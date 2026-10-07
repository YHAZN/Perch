"""Capture every Perch OS screen through the USB bridge as PNGs in captures/tour-*.png.

No AI calls. Run with the bridge up: python tools/tour.py
"""

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from shots import post, png  # noqa: E402

OUT = Path(__file__).resolve().parents[1] / 'captures'
steps = [
    ('face', '/api/button/home', None),
    ('apps', '/api/button/apps', None),
    ('control', '/api/button/control', None),
    ('camera', '/api/button/see', None),
    ('ask', '/api/button/ai', None),
    ('photos', '/api/button/photo', None),
    ('settings', '/api/button/status', None),
    ('model', None, {'x': 120, 'y': 136}),
    ('wifi', '/api/button/wifi', None),
    ('password', '/api/button/password-check', None),
    ('remote', '/api/button/remote', None),
    ('remote-media', None, {'x': 162, 'y': 211}),
    ('gestures', '/api/button/gestures', None),
]
for name, path, touch in steps:
    if path == '/api/button/control':
        post('/api/button/home')
    result = post(path) if path else post('/api/touch', touch)
    if name in ('camera', 'gestures'):
        time.sleep(4.5)  # let the LCD viewfinder run (the mirror pauses it while active)
        result = post('/api/screen')
    png(result, OUT / f'tour-{name}.png')
    print(f'{name:14s} view={result["view"]:10s} recovered={result.get("recovered")}')
    if path == '/api/button/control':
        post('/api/button/control')
post('/api/button/home')
