# Jarvis

A custom home automation pipeline that lets a Flipper Zero control Home Assistant via encrypted Sub-GHz RF.

```
Flipper Zero  →  433MHz RF (AES-256-GCM)  →  ESP32-C6 + CC1101  →  USB Serial  →  Gateway  →  MQTT  →  Home Assistant
```

## How it works

A Flipper Zero runs a custom app with a menu of rooms and commands. Pressing a button encrypts the command into a 15-byte packet using AES-256-GCM and transmits it at 433.92MHz. An ESP32-C6 with a CC1101 module receives the packet, verifies the authentication tag, and prints the decoded event over USB serial. A Python gateway daemon reads those events and publishes them to MQTT, where Home Assistant picks them up and triggers automations.

Rooms and commands are defined in a config file on the Flipper's SD card — no reflash needed to add or rename anything.

## Hardware

- Flipper Zero
- ESP32-C6 DevKitC (or similar)
- CC1101 433MHz SMA module
- Breadboard + Dupont wires

**CC1101 wiring (ESP32-C6):**

| CC1101 | ESP32-C6 GPIO |
|--------|--------------|
| MISO   | GPIO2        |
| SCK    | GPIO6        |
| MOSI   | GPIO7        |
| CSN    | GPIO10       |
| GDO0   | GPIO1        |
| VCC    | 3.3V         |
| GND    | GND          |

## Security

- AES-256-GCM encryption with a shared 32-byte key
- 8-byte truncated authentication tag on every packet
- Rolling counter for replay protection
- Counter persisted to Flipper SD card across app restarts

## Repository structure

```
flipper/        Flipper Zero app source (ufbt)
esp32/          ESP32-C6 firmware source (ESP-IDF)
gateway/        Python gateway daemon + systemd service
tools/          upload_config.py, monitor.py
docs/           Config update instructions
```

## Setup

### 1. Generate a shared key

```bash
head -c 32 /dev/urandom | xxd -i
```

Paste the output into `JARVIS_KEY` in both `flipper/jarvis_crypto.c` and `esp32/main/jarvis_crypto.c`. They must match.

### 2. Flash the ESP32

```bash
cd esp32
idf.py -p /dev/ttyUSB0 flash
```

### 3. Build and install the Flipper app

```bash
cd flipper
ufbt launch
```

### 4. Upload the config to the Flipper SD card

Edit `flipper/sdcard/config.txt` with your rooms and commands, then:

```bash
python3 tools/upload_config.py
```

### 5. Run the gateway

```bash
cp gateway/jarvis-gateway.env.example ~/jarvis_gateway/jarvis-gateway.env
# edit jarvis-gateway.env with your MQTT credentials
sudo systemctl enable --now jarvis-gateway
```

### 6. Add Home Assistant automations

Subscribe to MQTT topics in the format:

```
jarvis/event/<room_slug>/<command_slug>
```

Example for a "Bedroom / Lights On" command:

```yaml
alias: "Jarvis - Bedroom Lights On"
trigger:
  - platform: mqtt
    topic: jarvis/event/bedroom/lights_on
action:
  - service: light.turn_on
    target:
      entity_id: light.bedroom
mode: single
```

## Config file format

```
# room:<id>:<name>
# cmd:<id>:<name>

room:0:Living Room
cmd:0:Lights On
cmd:1:Lights Off

room:1:Bedroom
cmd:0:Lights On
cmd:1:Lights Off
```

IDs (0-255) are what the RF packet carries. Names are display labels only — renaming never requires reflashing either side. See `docs/update_config_instructions.txt` for the full update workflow.
