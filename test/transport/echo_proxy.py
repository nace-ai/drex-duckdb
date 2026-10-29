#!/usr/bin/env python3
"""An HTTP proxy stand-in that answers every POST with what it received, as JSON. It tunnels https to openrouter.ai
itself, with the mock's self-signed certificate, which it writes to the path in its second argument.

Run: python3 test/transport/echo_proxy.py <port> <certificate path>
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "mock"))
from drex_mock import OpenRouterTls, OpenRouterTunnel  # noqa: E402


class Echo(OpenRouterTunnel, BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_POST(self):  # noqa: N802
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        # A request through the tunnel names only its path
        url = f"https://{self.tunnel_host}{self.path}" if self.tunnel_host else self.path
        payload = json.dumps(
            {"url": url, "headers": {k.lower(): v for k, v in self.headers.items()}, "body": json.loads(body)}
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


Path(sys.argv[2]).write_bytes(OpenRouterTls.certificate())
ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Echo).serve_forever()
