"""Serve the bench wiring page and let it save wiring.json back to this folder.

    python tools/bench-wiring/serve.py [port]    # default 8001, localhost only

Like `python -m http.server`, plus PUT /wiring.json (valid JSON only) so the page's Save writes the file it was
served from. Nothing else is writable. Requests must name this machine as their Host (localhost or 127.0.0.1), so
a web page elsewhere can't reach the server through a DNS name rebound to 127.0.0.1.
"""
import functools
import http.server
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(HERE, 'wiring.json')


LOCAL_HOSTS = ('localhost', '127.0.0.1')


class Handler(http.server.SimpleHTTPRequestHandler):
    def _local_host(self):
        host = (self.headers.get('Host') or '').rsplit(':', 1)[0].lower()
        if host in LOCAL_HOSTS:
            return True
        self.send_error(403, 'Only localhost may use this server')
        return False

    def do_GET(self):
        if self._local_host():
            super().do_GET()

    def do_HEAD(self):
        if self._local_host():
            super().do_HEAD()

    def end_headers(self):
        # Tells the page that Save can write here, without a file dialog.
        if self.path.split('?')[0] == '/wiring.json':
            self.send_header('X-Bench-Wiring-Save', '1')
        super().end_headers()

    def do_PUT(self):
        if not self._local_host():
            return
        if self.path.split('?')[0] != '/wiring.json':
            return self.send_error(403, 'Only wiring.json can be saved')
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        try:
            json.loads(body)
        except ValueError as e:
            return self.send_error(400, f'Not valid JSON: {e}')
        tmp = TARGET + '.tmp'
        with open(tmp, 'wb') as f:
            f.write(body)
        os.replace(tmp, TARGET)  # a failed write never leaves a half-written file
        self.send_response(204)
        self.end_headers()


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8001
    server = http.server.ThreadingHTTPServer(('127.0.0.1', port), functools.partial(Handler, directory=HERE))
    print(f'Bench wiring: http://localhost:{port}  (saves to {TARGET})')
    server.serve_forever()
