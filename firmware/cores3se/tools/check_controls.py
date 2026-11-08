"""Check HTTP validation without sending charger writes.

Locks controls and leaves polling enabled. Reads the device key locally; never
prints it. Optional manual read exercises the command queue using read frames.
"""
import argparse
import json
from pathlib import Path
import re
import time
import requests

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("host")
parser.add_argument("--expect-stale", action="store_true")
parser.add_argument("--read", action="store_true")
parser.add_argument("--output", type=Path)
args = parser.parse_args()
if not re.fullmatch(r"[a-zA-Z0-9.-]+", args.host):
    parser.error("Use an IP address or hostname")
key = re.search(r'^#define DEVICE_TOKEN "([A-Fa-f0-9]{48})"',
                (root / "include/device_token.h").read_text(), re.M)[1]
session = requests.Session()
session.trust_env = False
base = f"http://{args.host}"
results = []

def check(name, data, expected, path="/api/control", auth=True):
    response = session.post(base + path, data=data,
                            headers={"X-NPB-Token": key} if auth else {}, timeout=8)
    assert response.status_code == expected, (name, response.status_code, response.text)
    results.append({"test": name, "http": response.status_code, "response": response.json()})
    print("PASS:", name, flush=True)
    return response.json()

check("lock controls", {"action": "lock"}, 200)
try:
    check("unauthenticated control denied", {"action": "operation", "enabled": "true"}, 403, auth=False)
    check("unauthenticated raw denied", {"can_id": "0xC0100", "data": "0 0 1"}, 403, "/api/raw", False)
    for enabled in ("true", "false"):
        check("locked operation " + enabled, {"action": "operation", "enabled": enabled}, 409)
    for value in ("20.99", "42.01", "28.401", "nan", "inf", "28junk", ""):
        check("invalid voltage " + value, {"action": "set_psu_voltage", "value": value}, 400)
    check("invalid current", {"action": "set_curve_cc", "value": "13.51"}, 400)
    check("mode requires confirmation", {"action": "initialize_psu"}, 400)
    check("reserved system bits rejected", {"action": "system_config_raw", "low": "0xff", "high": "0xff", "confirm": "true"}, 400)
    check("raw byte overflow", {"can_id": "0xC0100", "data": "0x60 256"}, 400, "/api/raw")
    check("arbitrary raw read rejected", {"can_id": "0x123", "data": "0x60 0"}, 409, "/api/raw")
    state = session.get(base + "/api/status", timeout=8).json()
    if args.expect_stale:
        assert not state["can_online"] and not state["status_fresh"]
        assert all(state[name] is None for name in ("voltage_v", "current_a", "temperature_c", "power_w", "operation_on"))
        check("stale unlock denied", {"action": "unlock"}, 409)
    if args.read:
        queued = check("manual voltage read", {"action": "read_command", "command": "0x0060"}, 202)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            state = session.get(base + "/api/status", timeout=8).json()
            if state["control"]["id"] == queued["id"] and state["control"]["state"] == "reads_sent":
                break
            time.sleep(.25)
        else:
            raise AssertionError(state["control"])
        assert state["can_online"]
        print("PASS: read-only command transmitted; live replies present")
    if args.output:
        args.output.write_text(json.dumps({"tests": results, "status": state}, indent=2))
finally:
    check("leave polling enabled", {"action": "set_paused", "paused": "false"}, 200)
    check("leave controls locked", {"action": "lock"}, 200)
