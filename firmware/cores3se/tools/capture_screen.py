"""Save an authenticated, read-only screenshot; never print the device key."""
import argparse
from pathlib import Path
import re
import requests

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('host')
parser.add_argument('output', type=Path)
args = parser.parse_args()
if args.output.suffix.lower() != '.bmp':
    parser.error('Use a .bmp output path')
if not re.fullmatch(r'[a-zA-Z0-9.-]+', args.host):
    parser.error('Use an IP address or hostname')
root = Path(__file__).resolve().parents[1]
key = re.search(r'^#define DEVICE_TOKEN "([A-Fa-f0-9]{48})"', (root / 'include/device_token.h').read_text(), re.M)[1]
session = requests.Session()
session.trust_env = False
response = session.get(f'http://{args.host}/api/screen.bmp', headers={'X-NPB-Token': key}, timeout=8)
response.raise_for_status()
args.output.write_bytes(response.content)
print('Saved', args.output)
