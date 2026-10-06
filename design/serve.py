"""Serve the design prototype on http://127.0.0.1:8766/ (loopback only).

Only design/ files and captures/*.jpg are reachable, never source or secrets.
"""

from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def do_GET(self):
        path = self.path.split('?')[0]
        if path == '/':
            self.path = '/design/index.html'
        elif (
            not (path.startswith('/design/') or (path.startswith('/captures/') and path.endswith('.jpg')))
            or '..' in path
        ):
            return self.send_error(404)
        return super().do_GET()


if __name__ == '__main__':
    print('Design prototype: http://127.0.0.1:8766/', flush=True)
    ThreadingHTTPServer(('127.0.0.1', 8766), Handler).serve_forever()
