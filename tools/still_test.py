"""Take diagnostic stills over USB without touching the device's gallery or Ask photo.

Usage: python tools/still_test.py [gain[:ae:frames] ...]   e.g.  python tools/still_test.py 2 2:1:2
Each gain is a gainceiling_t value (0 = 2x, 1 = 4x, 2 = 8x, 3 = 16x, 4 = 32x).
Saves captures/still-g<gain>.jpg and prints the exposure/gain the sensor used.
Stop the bridge first (it holds the port).
"""

import sys
import time
from pathlib import Path

import serial
from serial.tools import list_ports

ROOT = Path(__file__).resolve().parents[1]
ports = [p.device for p in list_ports.comports() if p.vid == 0x303A]
if not ports:
    raise SystemExit('No XIAO found over USB.')
port = serial.Serial()
port.port, port.baudrate, port.timeout = ports[0], 115200, 0.5
port.dtr, port.rts = True, False  # set before opening: RTS resets the board
port.open()
time.sleep(0.3)
port.reset_input_buffer()

for spec in sys.argv[1:] or ['2']:
    gain, *rest = spec.split(':')
    port.write(f'y{gain}\n'.encode())
    time.sleep(0.2)
    if rest:
        port.write(f'6{rest[0]},{rest[1]}\n'.encode())
    time.sleep(0.3)
    port.write(b'j')
    deadline = time.monotonic() + 40
    while time.monotonic() < deadline:
        line = port.readline()
        text = line.decode('utf-8', errors='replace').strip()
        if text.startswith(('CAMERA', 'CAPTURE', 'STILL')):
            print(text)
        if line.startswith(b'JPEG_BEGIN '):
            size = int(line.split()[1])
            data = bytearray()
            while len(data) < size and time.monotonic() < deadline:
                data.extend(port.read(size - len(data)))
            if len(data) != size or data[:2] != b'\xff\xd8':
                raise SystemExit('Incomplete JPEG transfer')
            out = ROOT / 'captures' / f"still-{spec.replace(':', '_')}.jpg"
            out.write_bytes(data)
            print(f'saved {out.name} ({len(data)} bytes)')
            break
    else:
        print('no JPEG received')
