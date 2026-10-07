"""Local USB bridge for the temporary Tiny AI screen. Standard library + pyserial."""

import argparse
import base64
import json
import threading
import struct
from contextlib import contextmanager
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial
from serial.tools import list_ports

ROOT = Path(__file__).resolve().parents[1]
WEB = Path(__file__).resolve().parent
LOCK = threading.Lock()
PORT = None
CONNECTION = None
PREVIEW_LOCK = threading.Lock()
PREVIEW_CACHE = None
PREVIEW_TIME = 0.0


@contextmanager
def device_connection(name):
    # Keep USB open between button presses: closing/reopening can reset the ESP32.
    global CONNECTION
    if CONNECTION is None or not CONNECTION.is_open:
        CONNECTION = serial.Serial()
        CONNECTION.port = name
        CONNECTION.baudrate = 115200
        CONNECTION.timeout = 0.5
        CONNECTION.write_timeout = 2
        CONNECTION.dtr = True
        CONNECTION.rts = False
        CONNECTION.open()
        CONNECTION.set_buffer_size(rx_size=2_000_000, tx_size=8192)
        # Give the board the PC's clock: it has no clock battery and may be offline.
        CONNECTION.write(b'Z%d\n' % int(time.time()))
    try:
        yield CONNECTION
    except (serial.SerialException, OSError):
        CONNECTION.close()
        CONNECTION = None
        raise


def board_port():
    if PORT:
        return PORT
    candidates = [p.device for p in list_ports.comports() if p.vid in (0x303A, 0x2886)]
    if len(candidates) != 1:
        raise RuntimeError("Connect one XIAO over USB, or start with --port COM5.")
    return candidates[0]


def bitmap565(data):
    # Repack device pixels as a lossless BMP; no PC layout or graphics rendering.
    width = 240
    height = len(data) // (width * 2)
    pixels = bytearray()
    for row in range(height - 1, -1, -1):
        for column in range(width):
            offset = (row * width + column) * 2
            value = data[offset] | data[offset + 1] << 8
            r, g, b = (value >> 11) & 31, (value >> 5) & 63, value & 31
            pixels.extend((b * 255 // 31, g * 255 // 63, r * 255 // 31))
    header = b'BM' + struct.pack('<IHHI', 54 + len(pixels), 0, 0, 54)
    header += struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
    return header + pixels


@contextmanager
def device_lock():
    if not LOCK.acquire(timeout=10):
        raise RuntimeError('Device is busy. Wait for the current operation to finish.')
    try:
        yield
    finally:
        LOCK.release()


def read_transfer_end(connection, expected, deadline):
    line = bytearray()
    while time.monotonic() < deadline:
        part = connection.read(1)
        if not part:
            continue
        if part == b'\n':
            value = bytes(line).strip()
            line.clear()
            if not value:
                continue
            if value == expected:
                return
            raise RuntimeError('Unexpected transfer ending: %r' % value)
        line.extend(part)
        if len(line) > 256:
            raise RuntimeError('Transfer ending exceeded 256 bytes.')
    raise RuntimeError('Timed out waiting for transfer ending.')


def read_payload(connection, length, deadline, acknowledged):
    data = bytearray()
    while len(data) < length and time.monotonic() < deadline:
        chunk_length = min(256 if acknowledged else length, length - len(data))
        chunk = bytearray()
        chunk_deadline = min(deadline, time.monotonic() + 4)
        while len(chunk) < chunk_length and time.monotonic() < chunk_deadline:
            part = connection.read(chunk_length - len(chunk))
            if part:
                chunk.extend(part)
                chunk_deadline = min(deadline, time.monotonic() + 4)
        if len(chunk) != chunk_length:
            raise RuntimeError('USB payload stalled after %d of %d bytes.' % (len(data) + len(chunk), length))
        data.extend(chunk)
        if acknowledged:
            connection.write(b'+')
    return data


def exchange(command, capture=False, screen=False):
    with device_lock():
        name = board_port()
        log = []
        with device_connection(name) as connection:
            connection.dtr = True
            connection.reset_input_buffer()
            connection.write(command.encode('ascii'))
            deadline = time.monotonic() + (
                110 if command in ('a', 'q', 'Q') or command.startswith('T') else 45 if command in ('w', 'g') else 25
            )
            while time.monotonic() < deadline:
                raw = connection.readline()
                if not raw:
                    continue
                line = raw.decode('utf-8', errors='replace').strip()
                if screen and raw.startswith(b'SCREEN_BEGIN '):
                    fields = raw.split()
                    length = int(fields[1])
                    if length not in (240 * 240 * 2, 240 * 284 * 2) or len(fields) not in (7, 8, 10, 11):
                        raise RuntimeError('Unexpected screen format.')
                    data = read_payload(connection, length, deadline, False)
                    checksum = 2166136261
                    for value in data:
                        checksum = ((checksum ^ value) * 16777619) & 0xFFFFFFFF
                    if len(data) != length or checksum != int(fields[6], 16):
                        print(
                            'SCREEN MISMATCH command=%r view=%s got=%d/%d checksum=%08x expected=%s'
                            % (command[:1], fields[3].decode(), len(data), length, checksum, fields[6].decode()),
                            flush=True,
                        )
                        (ROOT / 'captures' / 'bad-frame.bin').write_bytes(bytes(data))
                        raise RuntimeError('Screen transfer incomplete. Press Refresh screen.')
                    read_transfer_end(connection, b'SCREEN_END', deadline)
                    result = bitmap565(data)
                    output = ROOT / 'captures' / 'device-screen.bmp'
                    output.parent.mkdir(exist_ok=True)
                    output.write_bytes(result)
                    return {
                        'port': name,
                        'bytes': length,
                        'sequence': int(fields[2]),
                        'live': len(fields) == 11 and fields[10] == b'1',
                        'keys': {'gemini': fields[8] == b'1', 'gpt': fields[9] == b'1'} if len(fields) >= 10 else {},
                        'provider': fields[7].decode() if len(fields) >= 8 else None,
                        'width': 240,
                        'height': length // 480,
                        'view': fields[3].decode(),
                        'page': int(fields[4]),
                        'pages': int(fields[5]),
                        'checksum': fields[6].decode(),
                        'diagnostics': '\n'.join(log),
                        'image': 'data:image/bmp;base64,' + base64.b64encode(result).decode(),
                    }
                if 'SCREEN_ERROR' in line:
                    raise RuntimeError(line)
                if capture and raw.startswith(b'JPEG_BEGIN '):
                    length = int(raw.split()[1])
                    if not 4 <= length <= 2_000_000:
                        raise RuntimeError("Unexpected image length.")
                    data = read_payload(connection, length, deadline, False)
                    if len(data) != length:
                        raise RuntimeError("JPEG transfer timed out. Try Capture again.")
                    if data[:2] != b'\xff\xd8' or data[-2:] != b'\xff\xd9':
                        raise RuntimeError("The camera returned an incomplete JPEG.")
                    read_transfer_end(connection, b'JPEG_END', deadline)
                    output = ROOT / 'captures' / 'camera-test.jpg'
                    output.parent.mkdir(exist_ok=True)
                    if command != 'n':
                        output.write_bytes(data)
                    return {
                        'port': name,
                        'bytes': length,
                        'image': 'data:image/jpeg;base64,' + base64.b64encode(data).decode('ascii'),
                    }
                if line:
                    log.append(line)
                if 'unavailable' in line or 'CAPTURE FAILED' in line:
                    raise RuntimeError(line)
                if command == 'e' and line == 'PREVIEW_STOPPED':
                    return {'port': name, 'stopped': True}
                if command == 's' and 'Type g' in line:
                    return {'port': name, 'log': '\n'.join(log)}
                if command in ('w', 'g') and any(s in line for s in ('test complete', 'Cannot join', 'timed out')):
                    return {'port': name, 'log': '\n'.join(log)}
        raise RuntimeError('No complete board response. Close other serial monitors and retry.\n' + '\n'.join(log))


class Handler(BaseHTTPRequestHandler):
    def reply(self, status, payload, mime='application/json'):
        data = json.dumps(payload).encode() if mime == 'application/json' else payload
        self.send_response(status)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
            # A browser navigation can abandon a completed request. The device
            # operation already finished; do not report or replay it as a failure.
            pass

    def allowed(self):
        host = self.headers.get('Host', '')
        origin = self.headers.get('Origin')
        return host in ('127.0.0.1:8765', 'localhost:8765') and (
            origin is None or origin in ('http://127.0.0.1:8765', 'http://localhost:8765')
        )

    def do_GET(self):
        if not self.allowed():
            return self.reply(403, {'error': 'Local access only.'})
        path = self.path.split('?')[0]
        files = {
            '/': ('index.html', 'text/html; charset=utf-8'),
            '/app.js': ('app.js', 'text/javascript; charset=utf-8'),
            '/style.css': ('style.css', 'text/css; charset=utf-8'),
        }
        if path == '/api/health':
            return self.reply(
                200, {'ready': True, 'ports': [p.device for p in list_ports.comports() if p.vid in (0x303A, 0x2886)]}
            )
        if path not in files:
            return self.reply(404, {'error': 'Not found.'})
        filename, mime = files[path]
        self.reply(200, (WEB / filename).read_bytes(), mime)

    def do_POST(self):
        if not self.allowed():
            return self.reply(403, {'error': 'Local access only.'})
        if self.path == '/api/device-preview':
            return self.reply(409, {'error': 'Preview has been upgraded. Refresh this tab.'})
        if self.path == '/api/preview':
            global PREVIEW_CACHE, PREVIEW_TIME
            try:
                with PREVIEW_LOCK:
                    if PREVIEW_CACHE is None or time.monotonic() - PREVIEW_TIME > 0.010:
                        PREVIEW_CACHE = exchange('n', capture=True)
                        PREVIEW_TIME = time.monotonic()
                    result = PREVIEW_CACHE
                return self.reply(200, result)
            except (serial.SerialException, RuntimeError, OSError, ValueError):
                return self.reply(503, {'error': 'Preview interrupted. Check USB and retry.'})
        if self.path == '/api/touch':
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 1 <= length <= 128:
                    return self.reply(400, {'error': 'Invalid touch.'})
                payload = json.loads(self.rfile.read(length))
                x, y = payload.get('x'), payload.get('y')
                if type(x) is not int or type(y) is not int or not (0 <= x < 240 and 0 <= y < 284):
                    return self.reply(400, {'error': 'Invalid touch.'})
                try:
                    result = exchange(f'T{x},{y}\n', screen=True)
                except RuntimeError as error:
                    if not any(
                        reason in str(error)
                        for reason in ('USB payload stalled', 'Screen transfer incomplete', 'transfer ending')
                    ):
                        raise
                    # Read the resulting screen rather than replaying a touch,
                    # which might otherwise capture/upload the photo twice.
                    result = exchange('f', screen=True)
                    result['recovered'] = True
                return self.reply(200, result)
            except (ValueError, AttributeError, serial.SerialException, RuntimeError, OSError):
                return self.reply(503, {'error': 'Touch request failed. Check USB and retry.'})
        if self.path == '/api/ai/key':
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 1 <= length <= 2048:
                    return self.reply(400, {'error': 'Invalid settings request.'})
                payload = json.loads(self.rfile.read(length))
                provider, key = payload.get('provider'), payload.get('key')
                if provider not in ('gemini', 'gpt') or not isinstance(key, str):
                    return self.reply(400, {'error': 'Choose Gemini or GPT and enter an API key.'})
                key = key.strip()
                if len(key) >= 2 and key[0] == key[-1] and key[0] in ('"', "'"):
                    key = key[1:-1]
                if len(key) > 512 or any(not 33 <= ord(c) <= 126 for c in key):
                    return self.reply(400, {'error': 'Paste only the API key value, without a label or spaces.'})
                result = exchange('K' + ('G' if provider == 'gemini' else 'O') + key + '\n', screen=True)
                key = None
                payload.clear()
                return self.reply(200, result)
            except (ValueError, AttributeError, serial.SerialException, RuntimeError, OSError):
                return self.reply(503, {'error': 'Could not save settings on the board. Check USB and retry.'})
        commands = {
            '/api/preview': 'n',
            '/api/preview/stop': 'e',
            '/api/status': 's',
            '/api/capture': 'j',
            '/api/original': 'o',
            '/api/wifi/hotspot': 'w',
            '/api/wifi/campus': 'g',
            '/api/device-preview': 'N',
            '/api/screen': 'f',
            '/api/button/capture': 'a',
            '/api/button/demo': 'd',
            '/api/button/up': 'u',
            '/api/button/down': 'v',
            '/api/button/home': 'h',
            '/api/button/photo': 'p',
            '/api/button/status': 'i',
            '/api/button/retry': 'q',
            '/api/button/last-answer': 'b',
            '/api/button/ai': 'A',
            '/api/button/history': 'H',
            '/api/button/capture-ask': 'Q',
            '/api/button/see': 'C',
            '/api/button/control': 'D',
            '/api/button/apps': 'M',
            '/api/button/remote': 'R',
            '/api/button/wifi': 'W',
            '/api/button/gestures': 'E',
            '/api/button/password-check': 'V',
            '/api/button/provider/gemini': 'G',
            '/api/button/provider/gpt': 'O',
            '/api/button/rotate': 'r',
            '/api/button/tap': 't',
            '/api/tuning/baseline': '1',
            '/api/tuning/quality': '2',
            '/api/tuning/detail': '3',
            '/api/tuning/exposure': '4',
            '/api/tuning/reddit': '5',
        }
        if self.path not in commands:
            return self.reply(404, {'error': 'Not found.'})
        try:
            is_screen = (
                self.path in ('/api/screen', '/api/device-preview')
                or self.path.startswith('/api/button/')
                or self.path.startswith('/api/tuning/')
            )
            try:
                result = exchange(
                    commands[self.path], self.path in ('/api/capture', '/api/original', '/api/preview'), is_screen
                )
            except RuntimeError as transfer_error:
                message = str(transfer_error)
                if is_screen and any(
                    text in message for text in ('USB payload stalled', 'Screen transfer incomplete', 'transfer ending')
                ):
                    print('Recovering existing device screen after: ' + message, flush=True)
                    result = exchange('f', screen=True)
                    result['recovered'] = True
                elif self.path == '/api/original' and 'busy' not in message.lower():
                    print('Retrying saved original after: ' + message, flush=True)
                    result = exchange('o', capture=True)
                else:
                    raise
            self.reply(200, result)
        except (serial.SerialException, RuntimeError, ValueError, OSError) as error:
            print('DEVICE ERROR: ' + str(error), flush=True)
            self.reply(503, {'error': str(error)})

    def log_message(self, format, *args):
        print(format % args, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', help='Override automatic XIAO port detection')
    PORT = parser.parse_args().port
    print('Tiny AI screen: http://127.0.0.1:8765', flush=True)
    ThreadingHTTPServer(('127.0.0.1', 8765), Handler).serve_forever()
