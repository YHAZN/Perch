"""Check a release firmware image carries no secrets before it is published anywhere.

Usage: python tools/check_release.py [path/to/firmware.bin]   (default: the release build)
Looks for the Wi-Fi test network values from include/wifi_secrets.h and for API-key
shapes. Prints only names and found / not found -- never a secret value.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
image = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / '.pio' / 'build' / 'release' / 'firmware.bin'
data = image.read_bytes()
problems = 0
secrets = ROOT / 'include' / 'wifi_secrets.h'
if secrets.exists():
    for name, value in re.findall(r'(\w+)\[\]\s*=\s*"([^"]*)"', secrets.read_text(encoding='utf-8', errors='replace')):
        if len(value) < 4 or value == 'your-network':
            continue
        found = value.encode() in data
        problems += found
        print(f'{name}: {"FOUND IN IMAGE" if found else "not in image"}')
for label, pattern in [
    ('Gemini-style key', rb'AIza[0-9A-Za-z_\-]{30,}'),
    ('OpenAI-style key', rb'sk-[0-9A-Za-z_\-]{20,}'),
]:
    found = re.search(pattern, data) is not None
    problems += found
    print(f'{label}: {"FOUND IN IMAGE" if found else "none"}')
print(f'{image.name}: {len(data)} bytes, {"NOT SAFE to publish" if problems else "no secrets found"}')
sys.exit(1 if problems else 0)
