"""Capture device screens through the USB bridge as PNGs (no AI calls)."""

import base64, json, struct, sys, urllib.request, zlib

ROOT = 'http://127.0.0.1:8765'


def post(path, data=None):
    request = urllib.request.Request(
        ROOT + path,
        data=json.dumps(data).encode() if data else b'',
        headers={'Content-Type': 'application/json'},
        method='POST',
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def png(result, path):
    bmp = base64.b64decode(result['image'].split(',')[1])
    w, h = struct.unpack('<ii', bmp[18:26])
    off = struct.unpack('<I', bmp[10:14])[0]
    row = (w * 3 + 3) & ~3
    raw = b''.join(
        b'\0'
        + b''.join(
            bytes(
                (
                    bmp[off + (h - 1 - y) * row + x * 3 + 2],
                    bmp[off + (h - 1 - y) * row + x * 3 + 1],
                    bmp[off + (h - 1 - y) * row + x * 3],
                )
            )
            for x in range(w)
        )
        for y in range(h)
    )
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, 'wb').write(
        b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(raw))
        + chunk(b'IEND', b'')
    )
