# Project Jarvis

> Home Automation — Flipper Zero

```
Flipper Zero ──[Sub-GHz RF]──► ESP32-C6 + CC1101 ──[USB Serial]──► gateway.py ──[MQTT]──► Home Assistant
```
<img width="202" height="360" alt="flipper_demo" src="https://github.com/user-attachments/assets/8e27edc9-f112-4fd2-aa84-a33232c99617" />

📖 **Full build writeup:** [Substack](https://nikhilkamble.substack.com/p/flipper-home-automation)

---

## How it works

A Flipper Zero runs a custom app with a scrollable menu of rooms and commands. Pressing a button encrypts the selection into a fixed-length packet using AES-256-GCM and transmits it over Sub-GHz RF. An ESP32-C6 with a CC1101 module receives the packet, verifies the authentication tag, checks the rolling counter for replay attacks, and prints the decoded event over USB serial. A Python gateway reads those events and publishes them to MQTT, where Home Assistant triggers automations.

---

## Security model

- **Cipher:** AES-256-GCM — confidentiality and authentication in a single pass
- **Replay protection:** Rolling uint32 counter used as the GCM IV; receiver rejects any packet with a counter at or below the last accepted value
- **Authentication:** Truncated GCM tag on every packet — no packet is accepted without passing MAC verification
- **Counter persistence:** Counter is written to the Flipper SD card after every transmission to survive app restarts

> ⚠️ The shared AES key must be generated independently and embedded at build time. **Never commit your key.** See setup instructions below.

---

## Hardware

| Component | Role |
|---|---|
| Flipper Zero | Transmitter — custom app, built-in Sub-GHz radio |
| ESP32-C6 DevKitC | Receiver host — native USB CDC, hardware AES |
| CC1101 433 MHz module | Receiver radio — SPI-connected to ESP32-C6 |

---

## Repository structure

```
flipper/          Flipper Zero app source (C, ufbt)
esp32/            ESP32-C6 firmware source (C, ESP-IDF + FreeRTOS)
gateway/          Python 3 gateway daemon
tools/            Config upload and serial monitor scripts
docs/             config guide
```

---

## Setup

### 1. Generate a shared key

```bash
head -c 32 /dev/urandom | xxd -i
```

Paste the output into `JARVIS_KEY[]` in **both** `flipper/jarvis_crypto.c` and `esp32/main/jarvis_crypto.c`. The two values must match exactly.

### 2. Flash the ESP32-C6

```bash
cd esp32
idf.py set-target esp32c6
idf.py -p /dev/ttyUSB0 flash
```

### 3. Build and install the Flipper app

```bash
cd flipper
ufbt launch
```

### 4. Define your rooms and commands

Edit `flipper/sdcard/config.txt` using the format in [`config.txt.example`](flipper/sdcard/config.txt), then upload it:

```bash
python3 tools/upload_config.py
```

Apply the same room and command IDs to `gateway/config.yaml`. The gateway hot-reloads this file without a restart.

### 5. Run the gateway

```bash
cd gateway
pip install -r requirements.txt
cp jarvis-gateway.env.example jarvis-gateway.env
# Fill in MQTT_HOST, MQTT_USER, MQTT_PASS, SERIAL_PORT
sudo systemctl enable --now jarvis-gateway
```

### 6. Home Assistant

Subscribe to MQTT topics in the format:

```
jarvis/event/<room>/<command>
```

Payload is JSON with `room`, `command`, `counter`, and `rssi` fields.

---

## Config file format

```
room:<id>:<display_name>
cmd:<id>:<display_name>
```

IDs are what the RF packet carries. Names are display labels only — renaming never requires reflashing either side.

---

## License

| Path | License |
|---|---|
| `flipper/` | GPL v3 (inherits from Flipper Zero and Momentum firmware) |
| `esp32/` | GPL v3 |
| `gateway/` | MIT |

See `LICENSE-GPL` and `LICENSE-MIT`.

---

## References

- [CC1101 Datasheet — TI SWRS061](https://www.ti.com/lit/ds/symlink/cc1101.pdf)
- [Momentum Firmware — Next-Flip](https://github.com/Next-Flip/Momentum-Firmware)
- [ESP-IDF — Espressif](https://github.com/espressif/esp-idf)
- [NIST SP 800-38D — AES-GCM](https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38d.pdf)
- [MQTT v3.1.1 — OASIS](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html)
- [Eclipse Mosquitto](https://mosquitto.org)
- [Home Assistant MQTT integration](https://www.home-assistant.io/integrations/mqtt/)
