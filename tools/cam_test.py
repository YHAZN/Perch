"""Camera smoke test through the bridge: viewfinder screen, one capture, original JPEG check."""

import base64, sys, time

sys.path.insert(0, 'tools')
from shots import post, png

r = post('/api/button/see')
time.sleep(2)
png(post('/api/screen'), 'captures/camtest-viewfinder.png')
print('viewfinder view:', r['view'])
t = time.monotonic()
r = post('/api/button/capture')
print('capture view:', r['view'], f'{time.monotonic() - t:.2f}s')
png(post('/api/screen'), 'captures/camtest-photo.png')
jpg = base64.b64decode(post('/api/original')['image'].split(',')[-1])
open('captures/camtest-original.jpg', 'wb').write(jpg)
w = h = None
i = 2
while i < len(jpg) - 9:
    if jpg[i] == 0xFF and jpg[i + 1] in (0xC0, 0xC2):
        h, w = int.from_bytes(jpg[i + 5 : i + 7], 'big'), int.from_bytes(jpg[i + 7 : i + 9], 'big')
        break
    i += 1
print(
    'original:',
    len(jpg),
    'bytes',
    f'{w}x{h}',
    'valid' if jpg[:2] == b'\xff\xd8' and jpg[-2:] == b'\xff\xd9' else 'INVALID',
)
