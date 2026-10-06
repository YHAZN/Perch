"""Hardware regression through the USB bridge. Never sends an AI request.

Run with the bridge up: python tools/regress_no_ai.py
"""

import base64, json, time, urllib.request
from pathlib import Path

ROOT = 'http://127.0.0.1:8765'
OUT = Path(__file__).resolve().parents[1] / 'captures'


def post(path, data=None):
    # Only send a body when the route reads one; unread bytes make Windows reset the socket.
    req = urllib.request.Request(
        ROOT + path,
        data=json.dumps(data).encode() if data else b'',
        headers={'Content-Type': 'application/json'},
        method='POST',
    )
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def check(name, ok, detail=''):
    print(('PASS ' if ok else 'FAIL ') + name + (' | ' + detail if detail else ''))
    return ok


def save(result, name):
    OUT.mkdir(exist_ok=True)
    (OUT / name).write_bytes(base64.b64decode(result['image'].split(',')[-1]))


results = []
r = post('/api/button/home')
results.append(check('launcher', r['view'] == 'desk'))
for (x, y), view in [
    ((64, 48), 'home'),
    ((176, 48), 'ai'),
    ((64, 132), 'photo'),
    ((176, 132), 'history'),
    ((120, 216), 'status'),
]:
    post('/api/button/home')
    r = post('/api/touch', {'x': x, 'y': y})
    results.append(check('open ' + view, r['view'] == view))
    save(r, 'regress-' + view + '.bmp')
    r = post('/api/touch', {'x': 120, 'y': 279})
    results.append(check('home from ' + view, r['view'] == 'desk'))

# Provider toggle must stay in Settings now (it used to jump to Camera).
post('/api/button/home')
post('/api/touch', {'x': 120, 'y': 216})
before = post('/api/screen')['provider']
r = post('/api/touch', {'x': 120, 'y': 70})
results.append(
    check(
        'provider toggle stays in Settings',
        r['view'] == 'status' and r['provider'] != before,
        f"{before} -> {r['provider']}",
    )
)
r = post('/api/touch', {'x': 120, 'y': 70})
results.append(check('provider restored', r['provider'] == before))

# Camera-only capture, no network.
post('/api/button/home')
post('/api/touch', {'x': 64, 'y': 48})
start = time.monotonic()
r = post('/api/touch', {'x': 120, 'y': 246})
results.append(check('shutter opens Photos', r['view'] == 'photo', f'{time.monotonic() - start:.2f}s'))
original = base64.b64decode(post('/api/original')['image'].split(',')[-1])
results.append(
    check('original JPEG valid', original[:2] == b'\xff\xd8' and original[-2:] == b'\xff\xd9', f'{len(original)} bytes')
)
post('/api/button/home')
print(f"{sum(results)}/{len(results)} passed")
