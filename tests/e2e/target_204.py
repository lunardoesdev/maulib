#!/usr/bin/env python3
"""Tiny HTTP target for the e2e tests.

Endpoints:
  /204                      -> 204
  /ip                       -> the client IP as seen by the server
  /big?bytes=N              -> N bytes of zeros
  /slow?bytes=N&chunks=K&delay=S -> N bytes split into K chunks with delays
"""

import argparse
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _params(self):
        parsed = urlparse(self.path)
        return parsed.path, parse_qs(parsed.query)

    def _reply(self):
        path, params = self._params()

        if path == "/204":
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        if path == "/ip":
            body = (self.client_address[0] + "\n").encode()
            self._body(body)
            return

        if path == "/big":
            total = int(params.get("bytes", ["1048576"])[0])
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(total))
            self.end_headers()
            if self.command != "HEAD":
                self._write_zeros(total)
            return

        if path == "/slow":
            total = int(params.get("bytes", ["262144"])[0])
            chunks = max(1, int(params.get("chunks", ["10"])[0]))
            delay = float(params.get("delay", ["0.2"])[0])
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(total))
            self.end_headers()
            if self.command != "HEAD":
                self._write_slow(total, chunks, delay)
            return

        body = b"mewnet target\n"
        self._body(body)

    def _body(self, body):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _write_zeros(self, total):
        chunk = b"\x00" * min(64 * 1024, total)
        remaining = total
        while remaining > 0:
            piece = chunk[:remaining]
            self.wfile.write(piece)
            remaining -= len(piece)

    def _write_slow(self, total, chunks, delay):
        size = max(1, total // chunks)
        remaining = total
        for _ in range(chunks):
            if remaining <= 0:
                break
            piece = b"\x00" * min(size, remaining)
            self.wfile.write(piece)
            self.wfile.flush()
            remaining -= len(piece)
            time.sleep(delay)
        if remaining > 0:
            self.wfile.write(b"\x00" * remaining)

    do_GET = _reply
    do_HEAD = _reply

    def log_message(self, fmt, *args):
        pass


def start(port=0):
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args()
    server, _ = start(args.port)
    print(server.server_address[1], flush=True)
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
