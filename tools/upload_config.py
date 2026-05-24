#!/usr/bin/env python3
"""Upload jarvis config.txt to Flipper Zero over USB serial CLI."""

import sys
import time
import serial

PORT      = "/dev/ttyACM0"
BAUD      = 115200
DEST      = "/ext/apps_data/jarvis/config.txt"
SRC       = "/home/youruser/flipper/jarvis/sdcard/config.txt"


def send(s: serial.Serial, cmd: str) -> str:
    s.write((cmd + "\r\n").encode())
    time.sleep(0.3)
    return s.read(s.in_waiting).decode("utf-8", errors="replace")


def wait_prompt(s: serial.Serial, timeout: float = 5.0) -> bool:
    deadline = time.time() + timeout
    buf = ""
    while time.time() < deadline:
        buf += s.read(s.in_waiting).decode("utf-8", errors="replace")
        if ">:" in buf:
            return True
        time.sleep(0.1)
    return False


def main() -> None:
    with open(SRC) as f:
        content = f.read()

    print(f"Opening {PORT}...")
    with serial.Serial(PORT, BAUD, timeout=1) as s:
        # wake the CLI
        s.write(b"\r\n")
        time.sleep(0.5)
        s.read(s.in_waiting)

        print("Creating directory...")
        out = send(s, "storage mkdir /ext/apps_data/jarvis")
        print(f"  {out.strip()}")

        print("Removing old config...")
        out = send(s, f"storage remove {DEST}")
        # "not found" is fine on first run
        print(f"  {out.strip()}")

        print(f"Writing {DEST}...")
        s.write(f"storage write {DEST}\r\n".encode())
        time.sleep(0.5)
        s.read(s.in_waiting)  # consume echo / prompt

        # send file content line by line
        for line in content.splitlines():
            s.write((line + "\n").encode())
            time.sleep(0.02)

        # Ctrl+C to finish write
        s.write(b"\x03")
        time.sleep(0.5)
        out = s.read(s.in_waiting).decode("utf-8", errors="replace")
        print(f"  {out.strip()}")

        # verify the file exists
        print("Verifying...")
        out = send(s, f"storage stat {DEST}")
        print(f"  {out.strip()}")

        if "Error" in out or "not found" in out.lower():
            print("FAILED — file not found after write")
            sys.exit(1)

        print("Done — config uploaded successfully")


if __name__ == "__main__":
    main()
