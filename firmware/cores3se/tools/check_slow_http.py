"""Regression test: incomplete HTTP headers must not reboot or interrupt CAN.

Sends no charger writes. Run with a connected charger and normal polling.
"""
import argparse
import json
from pathlib import Path
import re
import socket
import time
import requests

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("host")
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
if not re.fullmatch(r"[a-zA-Z0-9.-]+", args.host):
    parser.error("Use an IP address or hostname")
s = requests.Session()
s.trust_env = False
url = f"http://{args.host}/api/status"
before = s.get(url, timeout=5).json()
assert before["can_online"] and not before["poll_paused"]
assert before["uptime_ms"] > 15000, "Wait until the device has been up for 15 seconds"
with socket.create_connection((args.host, 80), timeout=5) as client:
    client.sendall(b"GET / HTTP/1.1\r\nHost: npb-monitor.local\r\n")
    time.sleep(6)
after = s.get(url, timeout=8).json()
args.output.write_text(json.dumps({"before": before, "after": after}, indent=2))
assert after["uptime_ms"] > before["uptime_ms"], "Controller rebooted"
assert after["rx_frames"] > before["rx_frames"] + 20, "Polling stopped"
assert after["can"]["rx_missed"] == before["can"]["rx_missed"], "Receive drops"
assert after["can"]["max_service_gap_ms"] < 50, "CAN task stalled"
print("PASS: incomplete HTTP request rejected without a reboot;")
print(after["rx_frames"] - before["rx_frames"], "new replies; max service gap",
      after["can"]["max_service_gap_ms"], "ms")
