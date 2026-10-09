"""Hardware regression through the USB bridge. Never sends an AI request.

Run with the bridge up: python tools/regression.py
Safe offline: an Ask without Wi-Fi only queues (and the queue is cleared at the end).
With Wi-Fi it would send, so the Ask step is skipped when the board reports a connection.
"""

import base64
import json
import sys
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
    ((46, 66), 'ai'),
    ((120, 66), 'home'),
    ((194, 66), 'photo'),
    ((46, 164), 'remote'),
    ((120, 164), 'gestures'),
    ((194, 164), 'status'),
]:
    post('/api/button/apps')
    check('grid opens ' + view, post('/api/touch', {'x': x, 'y': y})['view'] == view)

# Nested screen and back: Settings > AI > back arrow.
post('/api/button/status')
check('Settings row opens AI', post('/api/touch', {'x': 120, 'y': 136})['view'] == 'model')

# The provider is never changed here: a missed tap could leave the device on a paid key.
check('AI screen shows the provider', post('/api/screen')['provider'] in ('gemini', 'gpt'))
check('back arrow returns to Settings', post('/api/touch', {'x': 31, 'y': 31})['view'] == 'status')

# Edge gestures: a scroll that starts at the left edge stays in the app; the bottom line goes home.
post('/api/button/apps')
check(
    'swipe up from the bottom line: grid -> face',
    post('/api/drag', {'x1': 120, 'y1': 279, 'x2': 120, 'y2': 120, 'ms': 300})['view'] == 'face',
)
post('/api/button/status')
check(
    'vertical scroll at the left edge is not Back',
    post('/api/drag', {'x1': 10, 'y1': 220, 'x2': 12, 'y2': 80, 'ms': 400})['view'] == 'status',
)
check(
    'pull down from the top opens Control Center',
    post('/api/drag', {'x1': 120, 'y1': 4, 'x2': 120, 'y2': 200, 'ms': 300})['view'] == 'control',
)
check(
    'swipe up from the bottom line closes it',
    post('/api/drag', {'x1': 120, 'y1': 279, 'x2': 120, 'y2': 100, 'ms': 300})['view'] == 'status',
)

# Camera-only capture, original JPEG intact. Opt-in: it takes a real photo, which becomes the
# newest photo (and the one Ask uses). Run with --capture after warning whoever holds the device.
if '--capture' in sys.argv:
    start = time.monotonic()
    r = post('/api/button/capture')
    check('capture opens Photos', r['view'] == 'photo', f'{time.monotonic() - start:.2f}s')
    original = base64.b64decode(post('/api/original')['image'].split(',')[-1])
    check('original JPEG valid', original[:2] == b'\xff\xd8' and original[-2:] == b'\xff\xd9', f'{len(original)} bytes')
else:
    print('SKIP capture (takes a real photo; run with --capture)')

# Offline Ask queues instead of sending (skipped if the board is online).
status = post('/api/status').get('log', '')
if 'IP: 0.0.0.0' not in status:
    print('SKIP offline-queue check: board may be online and an Ask would send')
else:
    post('/api/button/ai')
    r = post('/api/touch', {'x': 120, 'y': 198})
    check('offline Ask stays in Ask (queued)', r['view'] == 'ai')
    post('/api/button/clear-queue')

check('answers still come from Gemini', post('/api/button/home')['provider'] == 'gemini')
print(f'{sum(results)}/{len(results)} passed')
