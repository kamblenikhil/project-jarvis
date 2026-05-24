#!/usr/bin/env python3
"""Jarvis gateway: reads decoded packets from the ESP32 over USB serial and
publishes them to MQTT for Home Assistant to consume.

Topic layout:
    jarvis/event/<device_id>/<command_id>       numeric (backwards-compat)
    jarvis/event/<room_slug>/<command_slug>     human-readable (from config)
    jarvis/status                               "online" | "offline" (LWT)

Payload fields: device_id, command_id, room_name, command_name, counter, rssi, ts

Config file (JARVIS_CONFIG env var, default: ./config.txt):
    room:<id>:<name>    — starts a room block
    cmd:<id>:<name>     — adds a command to the room above it
    # comment / blank lines ignored
Config is reloaded automatically when the file changes (checked per packet).
"""

import fcntl
import json
import os
import re
import signal
import socket
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

import paho.mqtt.client as mqtt
import serial

SERIAL_PORT   = os.environ.get("JARVIS_SERIAL", "/dev/ttyUSB0")
SERIAL_BAUD   = int(os.environ.get("JARVIS_BAUD", "115200"))
WATCHDOG_SECS = int(os.environ.get("JARVIS_WATCHDOG", "60"))
MQTT_HOST     = os.environ.get("MQTT_HOST", "localhost")
MQTT_PORT     = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_USER     = os.environ.get("MQTT_USER") or None
MQTT_PASS     = os.environ.get("MQTT_PASS") or None
TOPIC_BASE    = os.environ.get("JARVIS_TOPIC", "jarvis")
CONFIG_PATH   = os.environ.get(
    "JARVIS_CONFIG", str(Path(__file__).parent / "config.txt")
)

# Matches lines like: JARVIS device=1 cmd=0 counter=5 rssi=-45
LINE_RE = re.compile(
    r"^JARVIS device=(\d+) cmd=(\d+) counter=(\d+) rssi=(-?\d+)\s*$"
)


def log(msg: str) -> None:
    print(f"[{datetime.now().isoformat(timespec='seconds')}] {msg}", flush=True)


def slugify(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


# ── USB watchdog ──────────────────────────────────────

USBDEVFS_RESET = 0x5514

def usb_reset(tty_port: str) -> bool:
    """Software USB reset — equivalent to physical unplug/replug."""
    tty_name = Path(tty_port).name
    try:
        device_link = Path(f"/sys/class/tty/{tty_name}/device")
        if not device_link.exists():
            log(f"USB reset: no sysfs entry for {tty_name}")
            return False
        dev_dir = Path(re.sub(r":\d+\.\d+$", "", str(device_link.resolve())))
        busnum = int((dev_dir / "busnum").read_text())
        devnum = int((dev_dir / "devnum").read_text())
        usb_path = f"/dev/bus/usb/{busnum:03d}/{devnum:03d}"
        with open(usb_path, "wb") as f:
            fcntl.ioctl(f, USBDEVFS_RESET, 0)
        log(f"USB reset OK — {usb_path} ({tty_name})")
        return True
    except Exception as e:
        log(f"USB reset failed: {e!r}")
        return False


# ── Config ────────────────────────────────────────────

@dataclass
class JarvisConfig:
    rooms: dict = field(default_factory=dict)   # device_id → room_name
    cmds:  dict = field(default_factory=dict)   # device_id → {cmd_id → cmd_name}
    mtime: float = 0.0
    path:  str = ""

    def room_name(self, device_id: int) -> str:
        return self.rooms.get(device_id, f"device_{device_id}")

    def cmd_name(self, device_id: int, cmd_id: int) -> str:
        return self.cmds.get(device_id, {}).get(cmd_id, f"cmd_{cmd_id}")


def load_config(path: str) -> JarvisConfig:
    cfg = JarvisConfig(path=path)
    try:
        cfg.mtime = os.path.getmtime(path)
    except OSError:
        log(f"Config not found at {path} — using numeric IDs only")
        return cfg

    current_room_id = None
    try:
        with open(path) as f:
            for raw in f:
                line = raw.strip()
                if not line or line.startswith("#"):
                    continue
                if line.startswith("room:"):
                    parts = line[5:].split(":", 1)
                    if len(parts) != 2 or not parts[0] or not parts[1]:
                        continue
                    rid = int(parts[0])
                    cfg.rooms[rid] = parts[1]
                    cfg.cmds.setdefault(rid, {})
                    current_room_id = rid
                elif line.startswith("cmd:") and current_room_id is not None:
                    parts = line[4:].split(":", 1)
                    if len(parts) != 2 or not parts[0] or not parts[1]:
                        continue
                    cid = int(parts[0])
                    cfg.cmds[current_room_id][cid] = parts[1]
        log(f"Config loaded: {len(cfg.rooms)} room(s) from {path}")
    except Exception as e:
        log(f"Config parse error: {e!r} — using what was loaded so far")
    return cfg


def maybe_reload_config(cfg: JarvisConfig) -> JarvisConfig:
    try:
        mtime = os.path.getmtime(cfg.path)
    except OSError:
        return cfg
    if mtime != cfg.mtime:
        log("Config file changed, reloading")
        return load_config(cfg.path)
    return cfg


# ── MQTT / serial ─────────────────────────────────────

def connect_mqtt() -> mqtt.Client:
    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=f"jarvis-gateway-{socket.gethostname()}-{os.getpid()}",
    )
    if MQTT_USER:
        client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.will_set(f"{TOPIC_BASE}/status", "offline", qos=1, retain=True)
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.loop_start()
    client.publish(f"{TOPIC_BASE}/status", "online", qos=1, retain=True)
    log(f"MQTT connected to {MQTT_HOST}:{MQTT_PORT}")
    return client


def open_serial() -> serial.Serial:
    s = serial.Serial(SERIAL_PORT, SERIAL_BAUD, timeout=1)
    s.reset_input_buffer()
    log(f"Serial open {SERIAL_PORT} @ {SERIAL_BAUD}")
    return s


# ── Main loop ─────────────────────────────────────────

def main() -> None:
    stop = {"flag": False}

    def handle_sig(signum, _frame):
        log(f"Signal {signum}, shutting down")
        stop["flag"] = True

    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)

    cfg        = load_config(CONFIG_PATH)
    client     = None
    ser        = None
    last_rx    = time.monotonic()

    while not stop["flag"]:
        try:
            if client is None:
                client = connect_mqtt()
            if ser is None:
                ser = open_serial()
                last_rx = time.monotonic()

            line = ser.readline().decode("utf-8", errors="replace").rstrip()
            if not line:
                if time.monotonic() - last_rx > WATCHDOG_SECS:
                    log(f"Watchdog: no data for {WATCHDOG_SECS}s — resetting USB")
                    try:
                        ser.close()
                    except Exception:
                        pass
                    ser = None
                    usb_reset(SERIAL_PORT)
                    time.sleep(3)
                continue

            last_rx = time.monotonic()

            m = LINE_RE.match(line)
            if not m:
                if line.startswith(("#", "SIGNAL", "alive")):
                    log(f"esp32: {line}")
                else:
                    log(f"unparsed: {line}")
                continue

            cfg = maybe_reload_config(cfg)

            device_id, command_id, counter, rssi = (int(x) for x in m.groups())

            room_name = cfg.room_name(device_id)
            cmd_name  = cfg.cmd_name(device_id, command_id)

            payload = json.dumps({
                "device_id":    device_id,
                "command_id":   command_id,
                "room_name":    room_name,
                "command_name": cmd_name,
                "counter":      counter,
                "rssi":         rssi,
                "ts":           datetime.now(timezone.utc).isoformat(timespec="seconds"),
            })

            numeric_topic = f"{TOPIC_BASE}/event/{device_id}/{command_id}"
            named_topic   = f"{TOPIC_BASE}/event/{slugify(room_name)}/{slugify(cmd_name)}"

            client.publish(numeric_topic, payload, qos=0, retain=False)
            if named_topic != numeric_topic:
                client.publish(named_topic, payload, qos=0, retain=False)

            log(f"PUB {named_topic}  ({numeric_topic})  {payload}")

        except serial.SerialException as e:
            log(f"serial error: {e} — reopening in 2s")
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            time.sleep(2)
        except (OSError, mqtt.MQTTException, ConnectionError) as e:
            log(f"mqtt error: {e} — reconnecting in 5s")
            try:
                client.loop_stop()
                client.disconnect()
            except Exception:
                pass
            client = None
            time.sleep(5)
        except Exception as e:
            log(f"unexpected: {e!r}")
            time.sleep(1)

    if client:
        client.publish(f"{TOPIC_BASE}/status", "offline", qos=1, retain=True)
        client.loop_stop()
        client.disconnect()
    if ser:
        ser.close()
    log("clean exit")


if __name__ == "__main__":
    main()
