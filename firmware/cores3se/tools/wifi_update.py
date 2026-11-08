"""Upload an app binary to a known NPB Monitor over a trusted local network.

The update token stays in the ignored include/device_token.h file. No charger
commands are sent. The board must already run the authenticated update endpoint.
"""
import argparse
from pathlib import Path
import re
import time

import requests

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("host", help="Device IP address, e.g. 192.168.4.100")
parser.add_argument("--binary", type=Path, default=root / ".pio/build/cores3se/firmware.bin")
parser.add_argument("--token-file", type=Path, default=root / "include/device_token.h")
parser.add_argument("--check-auth", action="store_true", help="Verify unauthenticated uploads are rejected")
parser.add_argument("--expected-version", help="Require this version after restart")
args = parser.parse_args()
if not re.fullmatch(r"[a-zA-Z0-9.-]+", args.host):
    parser.error("Use an IP address or hostname, without a URL path")
url = f"http://{args.host}"
session = requests.Session()
session.trust_env = False
before = session.get(url + "/api/status", timeout=5)
before.raise_for_status()
before_at = time.monotonic()
print("Connected:", before.json().get("firmware", "unknown"), flush=True)
if args.check_auth:
    response = session.post(url + "/api/firmware", files={"firmware": ("denied.bin", b"not firmware")}, timeout=10)
    if response.status_code != 403:
        raise RuntimeError(f"Unauthenticated upload was not denied: HTTP {response.status_code}")
    print("PASS: upload without token denied (HTTP 403)")
    raise SystemExit(0)
expected = args.expected_version
if expected is None and args.binary == root / ".pio/build/cores3se/firmware.bin":
    version = re.search(r'VERSION = "([^"]+)"', (root / "include/web_service.h").read_text())
    expected = version[1] if version else None
match = re.search(r'^#define DEVICE_TOKEN "([A-Fa-f0-9]{48})"', args.token_file.read_text(), re.M)
if not match:
    raise RuntimeError("Missing local 48-digit update token")
with args.binary.open("rb") as image:
    response = session.post(url + "/api/firmware", headers={"X-NPB-Token": match[1]},
                            files={"firmware": ("firmware.bin", image, "application/octet-stream")}, timeout=(5, 120))
response.raise_for_status()
print("Update accepted; waiting for reboot", flush=True)
time.sleep(3)
for attempt in range(30):
    try:
        response = session.get(url + "/api/status", timeout=2)
        response.raise_for_status()
        state = response.json()
        uninterrupted_uptime = before.json()["uptime_ms"] + (time.monotonic() - before_at) * 1000
        if state["uptime_ms"] + 1500 < uninterrupted_uptime and (expected is None or state.get("firmware") == expected):
            print("Booted:", state.get("firmware"), "uptime_ms:", state["uptime_ms"], flush=True)
            break
    except (requests.RequestException, ValueError):
        pass
    time.sleep(1)
else:
    raise RuntimeError("Upload accepted, but a rebooted device was not observed within 90 seconds")
