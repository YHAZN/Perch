"""Soak test: screen transfers stay intact while Wi-Fi retries in the background.

Uses the background AI path only with GPT and no GPT key saved, so nothing is sent.
"""

import json, sys, time, urllib.request

ROOT = 'http://127.0.0.1:8765'
SECONDS = int(sys.argv[1]) if len(sys.argv) > 1 else 180


def post(path):
    request = urllib.request.Request(ROOT + path, data=b'', method='POST')
    with urllib.request.urlopen(request, timeout=120) as response:
        return json.load(response)


status = post('/api/screen')
if status['keys'].get('gpt'):
    sys.exit('A GPT key is saved; refusing to run (could make a paid call).')
provider = status['provider']
post('/api/button/provider/gpt')
frames = recovered = slow = 0
start = time.monotonic()
try:
    while time.monotonic() - start < SECONDS:
        for path in ('/api/button/ai', '/api/button/retry', '/api/screen', '/api/button/home'):
            t = time.monotonic()
            r = post(path)
            took = time.monotonic() - t
            frames += 1
            recovered += bool(r.get('recovered'))
            slow += took > 3
finally:
    post('/api/button/provider/' + provider)
print(f'{frames} frames in {time.monotonic() - start:.0f}s | corrupted+recovered: {recovered} | slower than 3s: {slow}')
