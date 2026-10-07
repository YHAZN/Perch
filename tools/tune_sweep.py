"""Photo tuning sweep over USB: same scene, one variant per still. Never touches the gallery.

Usage: python tools/tune_sweep.py [variant ...]     (default: all)
Then:  python tools/image_score.py captures/tune-*.jpg   (system Python)
Keep the device still and the scene unchanged while it runs. Stop the bridge first.
"""

import sys
import time
from pathlib import Path

import serial
from serial.tools import list_ports

ROOT = Path(__file__).resolve().parents[1]
DEFAULTS = ['y2', '61,2', '7dn=-100', '7sh=-100', '7hts=0', '7set=0', '7sat=-100']
VARIANTS = {
    'base': [],
    'gain4x_exp3': ['y1', '61,3'],
    'gain2x_exp4': ['y0', '61,4'],
    'settle3': ['7set=3'],
    'denoise4': ['7dn=4'],
    'denoise8': ['7dn=8'],
    'sat-1': ['7sat=-1'],
    'hts5000': ['7hts=5000'],
    'combo': ['y1', '61,3', '7set=3', '7dn=6', '7sat=-1'],
}

names = sys.argv[1:] or list(VARIANTS)
ports = [p.device for p in list_ports.comports() if p.vid == 0x303A]
if not ports:
    raise SystemExit('No XIAO found over USB.')
port = serial.Serial()
port.port, port.baudrate, port.timeout = ports[0], 115200, 0.5
port.dtr, port.rts = True, False  # set before opening: RTS resets the board
port.open()
time.sleep(0.3)
port.reset_input_buffer()

for name in names:
    for command in DEFAULTS + VARIANTS[name]:
        port.write((command + '\n').encode())
        time.sleep(0.1)
    port.reset_input_buffer()
    port.write(b'j')
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        line = port.readline()
        text = line.decode('utf-8', errors='replace').strip()
        if text.startswith(('CAMERA exposure', 'CAMERA focus', 'CAPTURE')):
            print(f'{name}: {text}')
        if line.startswith(b'JPEG_BEGIN '):
            size = int(line.split()[1])
            data = bytearray()
            while len(data) < size and time.monotonic() < deadline:
                data.extend(port.read(size - len(data)))
            if len(data) != size or data[:2] != b'\xff\xd8':
                print(f'{name}: incomplete JPEG')
                break
            (ROOT / 'captures' / f'tune-{name}.jpg').write_bytes(data)
            print(f'{name}: saved ({len(data)} bytes)')
            break
    else:
        print(f'{name}: no JPEG')
for command in DEFAULTS:  # leave the board on its defaults
    port.write((command + '\n').encode())
    time.sleep(0.1)
