"""Score camera stills so tuning is measured, not eyeballed.

Usage: python tools/image_score.py captures/still-*.jpg     (system Python: needs PIL + numpy)

  luma     mean brightness, 0-255
  stripes  vertical line strength: wobble of per-column brightness after removing the
           smooth trend (lower is better)
  grain    median local noise in flat areas (lower is better)
  cast     mean R/G and B/G ratios (1.0 = neutral grey balance overall)
"""

import sys

import numpy as np
from PIL import Image


def smooth(values, window):
    kernel = np.ones(window) / window
    padded = np.pad(values, window // 2, mode='edge')
    return np.convolve(padded, kernel, mode='valid')[: len(values)]


def score(path):
    rgb = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)
    luma = rgb @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    columns = luma.mean(axis=0)
    stripes = float(np.std(columns - smooth(columns, 31)))
    # Local noise: difference from a 5x5 box blur, in 32x32 tiles; flat tiles show grain.
    h, w = luma.shape
    blur = luma.copy()
    for axis in (0, 1):
        blur = np.apply_along_axis(lambda v: smooth(v, 5), axis, blur)
    residual = np.abs(luma - blur)
    tiles = []
    for y in range(0, h - 32, 32):
        for x in range(0, w - 32, 32):
            tile = luma[y : y + 32, x : x + 32]
            if tile.std() < 12:  # flat area: what is left is noise
                tiles.append(residual[y : y + 32, x : x + 32].mean())
    grain = float(np.median(tiles)) if tiles else float('nan')
    r, g, b = (rgb[..., i].mean() for i in range(3))
    return luma.mean(), stripes, grain, r / g, b / g


print(f"{'file':34} {'luma':>6} {'stripes':>8} {'grain':>6} {'R/G':>5} {'B/G':>5}")
for name in sys.argv[1:]:
    l, s, n, rg, bg = score(name)
    print(f'{name[-34:]:34} {l:6.1f} {s:8.2f} {n:6.2f} {rg:5.2f} {bg:5.2f}')
