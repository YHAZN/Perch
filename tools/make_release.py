"""Build a release and the files to publish for wireless updates. Publishes nothing itself.

Usage: python tools/make_release.py <github-owner>/<repo>
  1. builds the release firmware (no Wi-Fi test network compiled in)
  2. refuses to continue if tools/check_release.py finds a secret in it
  3. writes dist/perch-<version>.bin and dist/perch.json (version, url, size, sha256)
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
manifest = {
    'version': version,
    'url': f'https://github.com/{repo}/releases/download/v{version}/{name}',
    'size': len(data),
    'sha256': hashlib.sha256(data).hexdigest(),
}
(dist / 'perch.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
print(f'Attach dist/{name} and dist/perch.json to a GitHub release tagged v{version}.')
print(f'Device update address: https://github.com/{repo}/releases/latest/download/perch.json')
