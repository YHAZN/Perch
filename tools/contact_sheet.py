"""Stitch captures/tour-*.png into captures/tour-sheet.png (5 per row) for visual review.

Reads the simple PNGs written by tools/shots.py (8-bit RGB, filter 0 on every row).
"""

import struct
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / 'captures'
W, H, GAP, COLS = 240, 284, 8, 5


def read(path):
    data = path.read_bytes()
    pos, idat = 8, b''
    while pos < len(data):
        length = struct.unpack('>I', data[pos : pos + 4])[0]
        kind = data[pos + 4 : pos + 8]
        if kind == b'IDAT':
            idat += data[pos + 8 : pos + 8 + length]
        pos += 12 + length
    raw = zlib.decompress(idat)
    row = W * 3 + 1
    return [raw[y * row + 1 : (y + 1) * row] for y in range(H)]


files = sorted(OUT.glob('tour-*.png'), key=lambda p: p.stat().st_mtime)
files = [f for f in files if f.name != 'tour-sheet.png']
rows = (len(files) + COLS - 1) // COLS
sw, sh = COLS * W + (COLS + 1) * GAP, rows * H + (rows + 1) * GAP
canvas = [bytearray(b'\x30' * (sw * 3)) for _ in range(sh)]
for i, f in enumerate(files):
    img = read(f)
    ox, oy = GAP + (i % COLS) * (W + GAP), GAP + (i // COLS) * (H + GAP)
    for y in range(H):
        canvas[oy + y][ox * 3 : (ox + W) * 3] = img[y]
raw = b''.join(b'\0' + bytes(r) for r in canvas)
chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
(OUT / 'tour-sheet.png').write_bytes(
    b'\x89PNG\r\n\x1a\n'
    + chunk(b'IHDR', struct.pack('>IIBBBBB', sw, sh, 8, 2, 0, 0, 0))
    + chunk(b'IDAT', zlib.compress(raw))
    + chunk(b'IEND', b'')
)
print('sheet:', [f.stem for f in files])
