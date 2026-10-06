"""Serve the design prototype on http://127.0.0.1:8766/ (loopback only).

Only design/ files and captures/*.jpg are reachable, never source or secrets.
"""

import base64
import json
import urllib.request
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BRIDGE = 'http://127.0.0.1:8765'


def bridge(path):
    request = urllib.request.Request(BRIDGE + path, data=b'', method='POST')
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.load(response)


def live_frame():
    """One preview JPEG from the real board via the USB bridge (camera only, no AI)."""
    try:
        result = bridge('/api/preview')
    except urllib.error.HTTPError:
        bridge('/api/button/see')  # preview only runs inside the device Camera app
        result = bridge('/api/preview')
    return base64.b64decode(result['image'].split(',', 1)[1])


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def do_GET(self):
        path = self.path.split('?')[0]
        if path in ('/live', '/live/stop'):
            return self.live(path)
        if path == '/':
            self.path = '/design/index.html'
        elif (
            not (path.startswith('/design/') or (path.startswith('/captures/') and path.endswith('.jpg')))
            or '..' in path
        ):
            return self.send_error(404)
        return super().do_GET()

    def live(self, path):
        try:
            if path == '/live/stop':
                bridge('/api/preview/stop')
                data, mime = b'{}', 'application/json'
            else:
                data, mime = live_frame(), 'image/jpeg'
            status = 200
        except (OSError, ValueError, KeyError):
            data, mime, status = b'{"error":"Board bridge unavailable"}', 'application/json', 503
        self.send_response(status)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)


if __name__ == '__main__':
    print('Design prototype: http://127.0.0.1:8766/', flush=True)
    ThreadingHTTPServer(('127.0.0.1', 8766), Handler).serve_forever()
