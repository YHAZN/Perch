"""Hardware regression through the USB bridge. Never sends an AI request.

Run with the bridge up: python tools/regress_no_ai.py
Safe offline: an Ask without Wi-Fi only queues (and the queue is cleared at the end).
With Wi-Fi it would send, so the Ask step is skipped when the board reports a connection.
"""

import base64
import json
import time
import urllib.request

ROOT = 'http://127.0.0.1:8765'


def post(path, data=None):
    # Only send a body when the route reads one; unread bytes make Windows reset the socket.
    request = urllib.request.Request(
        ROOT + path,
        data=json.dumps(data).encode() if data else b'',
        headers={'Content-Type': 'application/json'},
        method='POST',
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


results = []


def check(name, ok, detail=''):
    print(('PASS ' if ok else 'FAIL ') + name + (' | ' + detail if detail else ''))
    results.append(ok)


# Every app opens from the mirror and the face is home.
for route, view in [
    ('/api/button/home', 'face'),
    ('/api/button/apps', 'apps'),
    ('/api/button/see', 'home'),
    ('/api/button/ai', 'ai'),
    ('/api/button/photo', 'photo'),
    ('/api/button/status', 'status'),
    ('/api/button/remote', 'remote'),
    ('/api/button/gestures', 'gestures'),
    ('/api/button/wifi', 'wifi'),
]:
    check('open ' + view, post(route)['view'] == view)
post('/api/button/home')
check('control center', post('/api/button/control')['view'] == 'control')
post('/api/button/control')

# Grid taps open apps (grid positions from buildApps).
for (x, y), view in [
    ((50, 62), 'home'),
    ((120, 62), 'ai'),
    ((190, 62), 'photo'),
    ((85, 128), 'remote'),
    ((155, 128), 'status'),
    ((120, 194), 'gestures'),
]:
    post('/api/button/apps')
    check('grid opens ' + view, post('/api/touch', {'x': x, 'y': y})['view'] == view)

# Nested screen and back: Settings > Model > back arrow.
post('/api/button/status')
check('Settings row opens Model', post('/api/touch', {'x': 120, 'y': 136})['view'] == 'model')
check('back arrow returns to Settings', post('/api/touch', {'x': 31, 'y': 31})['view'] == 'status')

# Model picker round trip.
before = post('/api/screen')['provider']
post('/api/touch', {'x': 120, 'y': 136})
post('/api/touch', {'x': 120, 'y': 148})  # GPT row
after = post('/api/screen')['provider']
post('/api/touch', {'x': 120, 'y': 136})
post('/api/touch', {'x': 120, 'y': 88})  # Gemini row
check(
    'model picker switches and restores',
    after != before and post('/api/screen')['provider'] == before,
    f'{before} -> {after} -> back',
)

# Camera-only capture, original JPEG intact.
start = time.monotonic()
r = post('/api/button/capture')
check('capture opens Photos', r['view'] == 'photo', f'{time.monotonic() - start:.2f}s')
original = base64.b64decode(post('/api/original')['image'].split(',')[-1])
check('original JPEG valid', original[:2] == b'\xff\xd8' and original[-2:] == b'\xff\xd9', f'{len(original)} bytes')

# Offline Ask queues instead of sending (skipped if the board is online).
status = post('/api/status').get('log', '')
if 'IP: 0.0.0.0' not in status:
    print('SKIP offline-queue check: board may be online and an Ask would send')
else:
    post('/api/button/ai')
    r = post('/api/touch', {'x': 120, 'y': 198})
    check('offline Ask stays in Ask (queued)', r['view'] == 'ai')
    post('/api/button/clear-queue')

post('/api/button/home')
print(f'{sum(results)}/{len(results)} passed')
