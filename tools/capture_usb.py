import serial, time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
with serial.Serial('COM5', 115200, timeout=1) as port:
    port.dtr = True
    port.reset_input_buffer()
    port.write(b'cccj')
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        line = port.readline()
        print(line.decode('utf-8', errors='replace').strip())
        if line.startswith(b'JPEG_BEGIN '):
            size = int(line.split()[1])
            data = bytearray()
            while len(data) < size and time.monotonic() < deadline:
                data.extend(port.read(size - len(data)))
            if len(data) != size:
                raise RuntimeError('Incomplete JPEG transfer')
            assert data[:2] == b'\xff\xd8' and data[-2:] == b'\xff\xd9'
            output = root / 'captures' / 'camera-test.jpg'
            output.parent.mkdir(exist_ok=True)
            output.write_bytes(data)
            print(f'Saved {len(data)} bytes to {output}')
            break
    else:
        raise RuntimeError('No JPEG received; see camera output above')
