# SPDX-License-Identifier: GPL-3.0-or-later
"""Live dashboard: tiny HTTP server with Server-Sent Events (stdlib only)."""
from __future__ import annotations

import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from importlib import resources
from urllib.parse import parse_qs, urlparse

from .devices import DigitalInput8


def _make_handler(segment, master_label):
    page = resources.files(__package__).joinpath("dashboard.html").read_bytes()

    class Handler(BaseHTTPRequestHandler):
        server_version = "ecatsim"

        def log_message(self, fmt, *args):  # keep the console for bus events
            pass

        def _json(self, obj, code=200):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = urlparse(self.path).path
            if path == "/":
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(page)))
                self.end_headers()
                self.wfile.write(page)
            elif path == "/api/state":
                self._json(dict(segment.snapshot(), master=master_label))
            elif path == "/events":
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                try:
                    while True:
                        data = json.dumps(dict(segment.snapshot(), master=master_label))
                        self.wfile.write(f"data: {data}\n\n".encode())
                        self.wfile.flush()
                        time.sleep(0.1)
                except (BrokenPipeError, ConnectionResetError):
                    return
            else:
                self._json({"error": "not found"}, 404)

        def do_POST(self):
            # Reject cross-site POSTs (a web page must not drive fault injection):
            # browsers always send Origin on cross-origin requests.
            origin = self.headers.get("Origin")
            if origin is not None and urlparse(origin).netloc != self.headers.get("Host", ""):
                return self._json({"error": "cross-origin request rejected"}, 403)
            url = urlparse(self.path)
            q = {k: v[-1] for k, v in parse_qs(url.query).items()}
            try:
                if url.path == "/api/di":
                    slave = int(q.get("slave", "1"))
                    if not 1 <= slave <= len(segment.escs):
                        return self._json({"error": "no such slave"}, 400)
                    dev = segment.escs[slave - 1].device
                    if not isinstance(dev, DigitalInput8):
                        return self._json({"error": "slave is not a digital input"}, 400)
                    with segment.lock:
                        dev.set_input(int(q["ch"]), q.get("on", "1") == "1")
                    return self._json({"ok": True})
                if url.path == "/api/cable":
                    after = q.get("after")
                    segment.cut_cable_after(None if after in (None, "", "restore") else int(after))
                    return self._json({"ok": True, "link_count": segment.link_count})
            except (KeyError, ValueError, IndexError) as exc:
                return self._json({"error": str(exc)}, 400)
            return self._json({"error": "not found"}, 404)

    return Handler


def start_dashboard(segment, host: str, port: int,
                    master_label: str = "EtherCAT master") -> ThreadingHTTPServer:
    server = ThreadingHTTPServer((host, port), _make_handler(segment, master_label))
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, name="dashboard", daemon=True).start()
    return server
