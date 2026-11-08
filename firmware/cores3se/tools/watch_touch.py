"""Observe physical touchscreen checks over Wi-Fi; sends no charger commands."""
import argparse
import json
from pathlib import Path
import re
import time
import requests

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('host')
parser.add_argument('--seconds', type=int, default=60)
args = parser.parse_args()
if not re.fullmatch(r'[a-zA-Z0-9.-]+', args.host):
    parser.error('Use an IP address or hostname')
root = Path(__file__).resolve().parents[1]
folder = root.parents[1] / 'logs' / 'touch-validation'
folder.mkdir(parents=True, exist_ok=True)
key = re.search(r'^#define DEVICE_TOKEN "([A-Fa-f0-9]{48})"', (root / 'include/device_token.h').read_text(), re.M)[1]
session = requests.Session()
session.trust_env = False
base = f'http://{args.host}'
previous = None
last_capture = 0
started = time.monotonic()
with (folder / 'observations.jsonl').open('a', encoding='utf-8') as log:
    while time.monotonic() - started < args.seconds:
        state = session.get(base + '/api/status', timeout=5).json()
        display = state['display']
        sample = {name: state[name] for name in ('uptime_ms', 'display', 'control', 'operation_on', 'current_a', 'voltage_v', 'controls_enabled', 'can')}
        log.write(json.dumps(sample) + '\n')
        marker = (display, state['control'], state['operation_on'], round(state['current_a'] or 0, 1))
        changed = marker != previous
        if changed:
            print(json.dumps({k: sample[k] for k in ('uptime_ms', 'display', 'control', 'operation_on', 'current_a', 'controls_enabled')}), flush=True)
        if (changed and display['confirming'] or display['holding']) and time.monotonic() - last_capture > .3:
            response = session.get(base + '/api/screen.bmp', headers={'X-NPB-Token': key}, timeout=5)
            if response.status_code == 200:
                label = 'holding' if display['holding'] else 'review'
                (folder / f"{state['uptime_ms']}-{label}.bmp").write_bytes(response.content)
            last_capture = time.monotonic()
        previous = marker
        time.sleep(.15)
print('Finished read-only observation', flush=True)
