#!/usr/bin/env python3
"""Raw serial monitor for the Jarvis ESP32 receiver.
Stop the jarvis-gateway service before running this:
    sudo systemctl stop jarvis-gateway
"""

import sys
import serial
from datetime import datetime

PORT = "/dev/ttyUSB0"
BAUD = 115200

print(f"Monitoring {PORT} @ {BAUD} — Ctrl+C to quit")
print("-" * 50)

with serial.Serial(PORT, BAUD, timeout=1) as s:
    s.reset_input_buffer()
    while True:
        try:
            line = s.readline().decode("utf-8", errors="replace").rstrip()
            if line:
                ts = datetime.now().isoformat(timespec="seconds")
                print(f"[{ts}] {line}")
        except KeyboardInterrupt:
            print("\nDone.")
            sys.exit(0)
