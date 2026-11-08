"""Capture a bounded serial log without toggling reset or sending commands."""
import argparse
from pathlib import Path
import time

import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--reset", action="store_true", help="Reset ESP32 USB Serial/JTAG before capture")
parser.add_argument("--port", required=True)
parser.add_argument("--seconds", type=float, default=20)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
with serial.Serial(port=None, baudrate=115200, timeout=0.25) as device:
    device.dtr = False
    device.rts = False
    device.port = args.port
    device.open()
    if args.reset:
        device.rts = True
        time.sleep(0.2)
        device.rts = False
        time.sleep(0.2)
    deadline = time.monotonic() + args.seconds
    print(f"Capturing {args.port} for {args.seconds:g} seconds", flush=True)
    with args.output.open("wb") as log:
        while time.monotonic() < deadline:
            chunk = device.read(device.in_waiting or 1)
            if chunk:
                log.write(chunk)
                log.flush()
                print(chunk.decode("utf-8", errors="replace"), end="", flush=True)
    print(f"\nSaved {args.output}")
