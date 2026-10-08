"""Build a release and the files to publish for wireless updates. Publishes nothing itself.

Usage: python tools/make_release.py <github-owner>/<repo>
  1. builds the release firmware (no Wi-Fi test network compiled in)
  2. refuses to continue if tools/check_release.py finds a secret in it
  3. signs it with keys/perch-update-private.pem (never committed; back it up)
  4. writes dist/perch-<version>.bin and dist/perch.json (version, url, size, sha256, signature)
Then create a GitHub release tagged v<version> and attach both files. Devices whose update
address is https://github.com/<owner>/<repo>/releases/latest/download/perch.json will offer it.
"""

import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if len(sys.argv) != 2 or '/' not in sys.argv[1]:
    raise SystemExit(__doc__)
repo = sys.argv[1]
version = re.search(r'PERCH_VERSION "([^"]+)"', (ROOT / 'include' / 'version.h').read_text()).group(1)
pio = Path.home() / '.platformio' / 'penv' / 'Scripts' / 'pio.exe'
subprocess.run([str(pio), 'run', '-e', 'release'], cwd=ROOT, check=True)
image = ROOT / '.pio' / 'build' / 'release' / 'firmware.bin'
check = subprocess.run([sys.executable, str(ROOT / 'tools' / 'check_release.py'), str(image)])
if check.returncode:
    raise SystemExit('Secret found in the image: not making a release.')
dist = ROOT / 'dist'
dist.mkdir(exist_ok=True)
name = f'perch-{version}.bin'
shutil.copy(image, dist / name)
data = image.read_bytes()
# Sign the image (ECDSA P-256 over SHA-256) with the private release key; devices verify it.
key = ROOT / 'keys' / 'perch-update-private.pem'
if not key.exists():
    raise SystemExit('No release key in keys/: cannot sign. Restore your backup of perch-update-private.pem.')
sig_file = dist / 'perch.sig'
subprocess.run(['openssl', 'dgst', '-sha256', '-sign', str(key), '-out', str(sig_file), str(image)], check=True)
subprocess.run(
    [
        'openssl',
        'dgst',
        '-sha256',
        '-verify',
        str(ROOT / 'keys' / 'perch-update-public.pem'),
        '-signature',
        str(sig_file),
        str(image),
    ],
    check=True,
)
import base64

signature = base64.b64encode(sig_file.read_bytes()).decode()
sig_file.unlink()
manifest = {
    'version': version,
    'url': f'https://github.com/{repo}/releases/download/v{version}/{name}',
    'size': len(data),
    'sha256': hashlib.sha256(data).hexdigest(),
    'signature': signature,
}
(dist / 'perch.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
print(f'Attach dist/{name} and dist/perch.json to a GitHub release tagged v{version}.')
print(f'Device update address: https://github.com/{repo}/releases/latest/download/perch.json')
