#!/usr/bin/env python3
"""TTFB / TTLB / speed metrics through a local HTTP server.

Covers the three paths that record metrics:
  * mewnet_probe   - the agent health check through the mixed inbound;
  * mewnet_test_link - discovery / manual link tests;
  * mewnet_link_ping - per-link ping.

No Xray and no network: the server answers both normal requests and absolute
URI requests (an HTTP forward proxy), which is enough for the http proxy link.
"""

import http.server
import os
import socket
import sys
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from test_api import LIB_PATH, Mewnet  # noqa: E402

BODY = b"x" * (256 * 1024)
PROBE_LIMIT = 64 * 1024


class PayloadHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):  # noqa: N802
        try:
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(BODY)))
            self.end_headers()
            self.wfile.write(BODY)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_CONNECT(self):  # noqa: N802
        """Minimal forward proxy tunnel: mihomo's http proxy uses CONNECT."""
        host, _, port = self.path.partition(":")
        try:
            upstream = socket.create_connection((host, int(port)), timeout=5)
        except OSError:
            self.send_error(502)
            return
        self.send_response(200, "Connection Established")
        self.end_headers()
        self.close_connection = True
        self.connection.settimeout(5)
        upstream.settimeout(5)

        def pipe(src, dst):
            try:
                while True:
                    chunk = src.recv(65536)
                    if not chunk:
                        break
                    dst.sendall(chunk)
            except OSError:
                pass
            finally:
                try:
                    dst.shutdown(socket.SHUT_WR)
                except OSError:
                    pass

        worker = threading.Thread(
            target=pipe, args=(self.connection, upstream), daemon=True
        )
        worker.start()
        pipe(upstream, self.connection)
        worker.join(timeout=5)
        upstream.close()

    def log_message(self, format, *args):  # noqa: A002
        pass


class MetricsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(LIB_PATH):
            raise unittest.SkipTest(f"{LIB_PATH} not built")
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), PayloadHandler)
        cls.server.daemon_threads = True
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.port = cls.server.server_address[1]
        cls.url = f"http://127.0.0.1:{cls.port}/payload"
        cls.link = f"http://127.0.0.1:{cls.port}#local-http"

        cls.api = Mewnet()
        cls.api.init(options={"mixed_port": 0, "log_level": "warning"})

    @classmethod
    def tearDownClass(cls):
        if getattr(cls, "api", None):
            cls.api.shutdown()
        if getattr(cls, "server", None):
            cls.server.shutdown()
            cls.server.server_close()

    def _check_metrics(self, result, label):
        self.assertTrue(result["ok"], f"{label}: {result}")
        self.assertGreaterEqual(result["ttfb"], 1, f"{label}: ttfb {result}")
        self.assertGreaterEqual(result["ttlb"], result["ttfb"], f"{label}: ttlb < ttfb {result}")
        self.assertEqual(result["delay"], result["ttlb"], f"{label}: delay != ttlb {result}")
        self.assertEqual(result["bytes"], PROBE_LIMIT, f"{label}: bytes {result}")
        self.assertGreater(result["speed_bps"], 0, f"{label}: speed {result}")

    def test_01_probe_through_inbound(self):
        result = self.api.probe(self.url, expected=200, timeout_ms=5000)
        self.assertEqual(result["status"], 200, result)
        self._check_metrics(result, "probe")

    def test_02_test_link(self):
        result = self.api.test_link(
            self.link, {"url": self.url, "expected": "200", "timeout_ms": 5000}
        )
        self._check_metrics(result, "test_link")

    def test_03_test_delay_by_name(self):
        added = self.api.add(self.link, {"select": False})
        self.assertTrue(added["ok"], added)
        name = added["added"][0]["name"]
        try:
            result = self.api.test_delay(
                name, {"url": self.url, "expected": "200", "timeout_ms": 5000}
            )
            self._check_metrics(result, "test_delay")
        finally:
            self.api.remove(name)

    def test_04_link_ping(self):
        handle = self.api.link_create(self.link, {"local_proxy": False})
        try:
            result = self.api.link_ping(
                handle, {"url": self.url, "expected": "200", "timeout_ms": 5000}
            )
            self._check_metrics(result, "link_ping")
        finally:
            self.api.link_destroy(handle)


if __name__ == "__main__":
    unittest.main(verbosity=2)
