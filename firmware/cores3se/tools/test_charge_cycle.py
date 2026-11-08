"""Explicit, supervised hardware test: brief charge, then OFF and lock.

Only run with the identified TimeUSB battery connected and confirmed warm.
Does not change charger curve/configuration. Logs readbacks without credentials.
"""
import argparse
import json
from pathlib import Path
import re
import time
import requests

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("host")
parser.add_argument("--battery-connected-and-warm", action="store_true", required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
if not re.fullmatch(r"[a-zA-Z0-9.-]+", args.host):
    parser.error("Use an IP address or hostname")
root = Path(__file__).resolve().parents[1]
key = re.search(r'^#define DEVICE_TOKEN "([A-Fa-f0-9]{48})"',
                (root / "include/device_token.h").read_text(), re.M)[1]
s = requests.Session()
s.trust_env = False
url = f"http://{args.host}"
record = []

def status():
    r = s.get(url + "/api/status", timeout=5)
    r.raise_for_status()
    return r.json()

def action(data):
    r = s.post(url + "/api/control", data=data, headers={"X-NPB-Token": key}, timeout=5)
    r.raise_for_status()
    return r.json()

def verified(job):
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        state = status()
        if state["control"]["id"] == job["id"]:
            if state["control"]["state"] == "failed":
                raise RuntimeError(state["control"])
            if state["control"]["state"] == "verified":
                record.append(state)
                print("Verified command", job["id"], state["control"]["completed_steps"], "steps", flush=True)
                return
        time.sleep(.2)
    raise RuntimeError("Command readback was not verified")

before = status()
assert before.get("charge_ready"), before.get("charge_block_reason")
assert before["can"]["tx_error_counter"] == before["can"]["rx_error_counter"] == 0
assert not before["ac_cycle_pending"]
registers = {r["command"]: r for r in before["registers"]}
assert registers[0xB0]["raw"] == 500 and registers[0xB1]["raw"] == 2840
record.append(before)
request = {"action": "charge_now", "confirm_battery": "true", "confirm_temperature": "true"}
for name, cmd in zip(("cc", "cv", "float", "taper", "restart", "curve_config", "system_config"),
                     (0xB0, 0xB1, 0xB2, 0xB3, 0xB9, 0xB4, 0xC2)):
    assert registers[cmd]["quality"] == "valid"
    request[name] = str(registers[cmd]["raw"])
action({"action": "unlock"})
try:
    verified(action(request))
    for _ in range(5):
        time.sleep(1)
        current = status()
        record.append(current)
        print({k: current[k] for k in ("voltage_v", "current_a", "phase", "operation_on", "fault_flags")}, flush=True)
        if not current["status_fresh"] or current["fault_flags"] & 0xBE:
            raise RuntimeError("Unexpected charger status; stopping")
finally:
    # Do not silently assume a failed stop succeeded.
    try:
        deadline = time.monotonic() + 7
        while status()["control"]["state"] in ("queued", "awaiting_readback", "sending_reads") and time.monotonic() < deadline:
            time.sleep(.2)
        verified(action({"action": "operation", "enabled": "false"}))
        time.sleep(3)
        after = status()
        record.append(after)
        assert after["operation_on"] is False and after["fault_flags"] & 64
        assert after["current_a"] is not None and after["current_a"] < .2
        print("PASS: Operation OFF, output-off status and negligible charging current confirmed", flush=True)
    finally:
        action({"action": "lock"})
        args.output.write_text(json.dumps(record, indent=2))
        print("Controls locked; hardware log saved", flush=True)
