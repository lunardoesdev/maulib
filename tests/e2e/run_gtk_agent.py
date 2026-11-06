#!/usr/bin/env python3
"""Automated reliability test for the mewnet-gtk discovery engine.

The GUI is not involved: it runs the same engine in --headless mode against
local HTTP proxies and a local 204 origin, so the test is fully offline.

Scenarios:
  1. bootstrap from a subscription (fast discovery)
  2. grow the verified pool in the background
  3. manual "change proxy" (SIGUSR1 in headless mode)
  4. active proxy dies -> health check -> automatic failover
  5. all proxies die -> aggressive discovery keeps retrying
  6. one proxy returns -> engine recovers and serves traffic again
  7. empty database -> wait for a subscription -> fetch and activate
"""

import json
import os
import re
import signal
import socket
import sqlite3
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BIN = os.environ.get("MEWNET_GTK_BIN", os.path.join(ROOT, "build", "apps", "mewnet-gtk", "mewnet-gtk"))

PASSED = []
FAILED = []


def check(name, condition, detail=""):
    if condition:
        PASSED.append(name)
        print(f"PASS  {name}")
    else:
        FAILED.append(name)
        print(f"FAIL  {name}  {detail}")


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Origin:
    def __init__(self):
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):  # noqa: N802
                body = b""
                self.send_response(204)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def url(self):
        return f"http://127.0.0.1:{self.port}/204"

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class ForwardProxy:
    """Minimal HTTP proxy. Supports CONNECT and absolute-form requests."""

    def __init__(self, upstream_port, port=0):
        self.upstream_port = upstream_port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(64)
        self.port = self.sock.getsockname()[1]
        self.running = True
        self.clients = set()
        self.lock = threading.Lock()
        threading.Thread(target=self._accept_loop, daemon=True).start()

    def _accept_loop(self):
        while self.running:
            try:
                client, _ = self.sock.accept()
            except OSError:
                break
            with self.lock:
                self.clients.add(client)
            threading.Thread(target=self._handle, args=(client,), daemon=True).start()

    def _handle(self, client):
        try:
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = client.recv(4096)
                if not chunk:
                    return
                data += chunk
            line = data.split(b"\r\n", 1)[0].decode("latin1")
            method, target, version = line.split(" ", 2)
            if method == "CONNECT":
                host, port = target.rsplit(":", 1)
                upstream = socket.create_connection((host, int(port)), timeout=3)
                client.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
                self._relay(client, upstream)
            else:
                parsed = urlsplit(target)
                host = parsed.hostname or "127.0.0.1"
                port = parsed.port or self.upstream_port
                path = parsed.path or "/"
                if parsed.query:
                    path += "?" + parsed.query
                upstream = socket.create_connection((host, port), timeout=3)
                headers = data.split(b"\r\n", 1)[1]
                upstream.sendall(f"{method} {path} {version}\r\n".encode() + headers)
                self._relay(client, upstream)
        except Exception:
            try:
                client.sendall(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n")
            except OSError:
                pass
        finally:
            with self.lock:
                self.clients.discard(client)
            try:
                client.close()
            except OSError:
                pass

    @staticmethod
    def _relay(client, upstream):
        def pump(src, dst):
            try:
                while True:
                    chunk = src.recv(65536)
                    if not chunk:
                        break
                    dst.sendall(chunk)
            except OSError:
                pass
            try:
                dst.shutdown(socket.SHUT_WR)
            except OSError:
                pass

        thread = threading.Thread(target=pump, args=(upstream, client), daemon=True)
        thread.start()
        pump(client, upstream)
        thread.join(timeout=1)
        try:
            upstream.close()
        except OSError:
            pass

    def stop(self):
        self.running = False
        try:
            self.sock.close()
        except OSError:
            pass
        with self.lock:
            for client in list(self.clients):
                try:
                    client.shutdown(socket.SHUT_RDWR)
                    client.close()
                except OSError:
                    pass
            self.clients.clear()


class SubscriptionServer:
    def __init__(self, links):
        self.links = list(links)

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):  # noqa: N802
                body = ("\n".join(self.server.links) + "\n").encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.links = self.links
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def url(self):
        return f"http://127.0.0.1:{self.port}/sub"

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class Headless:
    def __init__(self, args):
        self.proc = subprocess.Popen(
            [BIN, "--headless", *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        self.lines = []
        self.cond = threading.Condition()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.proc.stdout:
            with self.cond:
                self.lines.append(line.rstrip("\n"))
                self.cond.notify_all()

    def wait_for(self, pattern, timeout=20.0, start=0):
        deadline = time.time() + timeout
        rx = re.compile(pattern)
        with self.cond:
            while time.time() < deadline:
                for index in range(start, len(self.lines)):
                    if rx.search(self.lines[index]):
                        return index
                self.cond.wait(max(0.05, min(0.25, deadline - time.time())))
        return -1

    def wait_after(self, pattern, after, timeout=20.0):
        return self.wait_for(pattern, timeout=timeout, start=after + 1)

    def active_names(self):
        result = []
        for line in self.lines:
            match = re.search(r"ACTIVE id=\d+ name=(\S+)", line)
            if match:
                result.append(match.group(1))
        return result

    def proxy_port(self):
        for line in self.lines:
            match = re.search(r"PROXY http://127\.0\.0\.1:(\d+)", line)
            if match:
                return int(match.group(1))
        return 0

    def send(self, sig):
        self.proc.send_signal(sig)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)


def proxy_link(proxy, name):
    return f"http://127.0.0.1:{proxy.port}#{name}"


def proxy_index(name):
    match = re.match(r"proxy-(\d+)", name)
    return int(match.group(1)) if match else 0


def curl_status(proxy_port, target_url, timeout=6):
    proc = subprocess.run(
        [
            "curl", "-sS", "-o", "/dev/null", "-w", "%{http_code}",
            "--max-time", str(timeout), "--noproxy", "",
            "-x", f"http://127.0.0.1:{proxy_port}", target_url,
        ],
        capture_output=True,
        text=True,
    )
    return proc.returncode, proc.stdout.strip(), proc.stderr.strip()


def alive_count(db_path):
    try:
        db = sqlite3.connect(db_path)
        row = db.execute("SELECT COUNT(*) FROM proxies WHERE alive=1").fetchone()
        db.close()
        return row[0] if row else 0
    except sqlite3.Error:
        return 0


def insert_subscription(db_path, url):
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            db = sqlite3.connect(db_path)
            row = db.execute("SELECT id FROM subscription_groups ORDER BY id LIMIT 1").fetchone()
            if row:
                db.execute(
                    "INSERT INTO subscriptions("
                    "group_id,url,name,priority,enabled,fetch_interval_sec,next_fetch_at,"
                    "last_status,failure_count,etag,last_modified,userinfo,last_error,"
                    "generation,created_at) VALUES(?,?,?,?,1,3600,0,0,0,'','','','',0,?)",
                    (row[0], url, "", 100, int(time.time() * 1000)),
                )
                db.commit()
                db.close()
                return True
            db.close()
        except sqlite3.Error:
            pass
        time.sleep(0.2)
    return False


def wait_curl_ok(port, url, timeout=15):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        code, out, err = curl_status(port, url, timeout=3)
        last = (code, out, err)
        if code == 0 and out == "204":
            return True, last
        time.sleep(0.5)
    return False, last


def main():
    if not os.path.exists(BIN):
        print(f"SKIP: {BIN} not built")
        return 2

    work = os.path.join("/tmp", f"mewnet-gtk-agent-{os.getpid()}")
    os.makedirs(work, exist_ok=True)
    db_path = os.path.join(work, "agent.db")

    origin = Origin()
    proxies = [ForwardProxy(origin.port) for _ in range(3)]
    links = [proxy_link(proxy, f"proxy-{i + 1}") for i, proxy in enumerate(proxies)]
    subscription = SubscriptionServer(links)

    app = None
    try:
        app = Headless(
            [
                "--run-seconds", "120",
                "--db", db_path,
                "--proxy-port", "0",
                "--batch", "10",
                "--concurrency", "10",
                "--timeout", "1000",
                "--slow-interval", "1",
                "--fetch-interval", "3600",
                "--pool-target", "3",
                "--health-interval", "1",
                "--health-timeout", "700",
                "--health-url", origin.url(),
                "--health-expected", "204",
                "--test-url", origin.url(),
                "--test-expected", "204",
                subscription.url(),
            ]
        )

        ready = app.wait_for(r"READY local=", timeout=30)
        check("bootstrap: READY", ready >= 0)
        port = app.proxy_port()
        check("bootstrap: local proxy port", port > 0, f"port={port}")

        ok, detail = wait_curl_ok(port, origin.url())
        check("bootstrap: traffic through proxy", ok, str(detail))

        deadline = time.time() + 30
        while time.time() < deadline and alive_count(db_path) < 3:
            time.sleep(0.3)
        check("pool: grew to three working proxies", alive_count(db_path) >= 3,
              f"alive={alive_count(db_path)}")

        switched_names = set()
        switched = 0
        for _ in range(3):
            before = app.active_names()[-1] if app.active_names() else ""
            app.send(signal.SIGUSR1)
            deadline = time.time() + 15
            changed = False
            while time.time() < deadline:
                names = app.active_names()
                if names and names[-1] != before:
                    switched_names.add(proxy_index(names[-1]))
                    changed = True
                    break
                time.sleep(0.2)
            if not changed:
                break
            switched += 1
            ok, detail = wait_curl_ok(port, origin.url())
            if not ok:
                break
        check("switch: moved to another proxy", switched >= 1,
              f"switched={switched}")
        check("switch: random among best, not just two", len(switched_names) >= 3,
              f"targets={sorted(switched_names)}")
        ok, detail = wait_curl_ok(port, origin.url())
        check("switch: traffic still works", ok, str(detail))

        names = app.active_names()
        current = names[-1] if names else ""
        index = proxy_index(current)
        check("failover: active proxy identified", 1 <= index <= len(proxies), current)
        proxies[index - 1].stop()

        failover_seen = app.wait_for(r"HEALTH FAIL", timeout=20)
        check("failover: health check detected dead proxy", failover_seen >= 0)

        deadline = time.time() + 25
        moved = False
        while time.time() < deadline:
            names = app.active_names()
            if names and names[-1] != current:
                moved = True
                break
            time.sleep(0.2)
        check("failover: switched to another proxy", moved,
              f"current={current} names={app.active_names()[-3:]}")

        ok, detail = wait_curl_ok(port, origin.url())
        check("failover: traffic works after switch", ok, str(detail))

        for proxy in proxies:
            proxy.stop()

        fast = app.wait_for(r"BATCH reason=fast", timeout=25)
        check("exhausted: aggressive discovery started", fast >= 0)
        check("exhausted: process still alive", app.proc.poll() is None)

        # Publish a new working link and wait for the engine to discover it
        # after its current pool is exhausted.
        revived = ForwardProxy(origin.port)
        proxies.append(revived)
        subscription.links.append(proxy_link(revived, f"proxy-{len(proxies)}"))

        recovered = app.wait_for(r"ACTIVE id=", timeout=35,
                                 start=(fast if fast >= 0 else 0))
        check("recovery: active proxy selected again", recovered >= 0)
        ok, detail = wait_curl_ok(port, origin.url(), timeout=25)
        check("recovery: traffic works again", ok, str(detail))

        app.stop()
        app = None

        # Empty database: wait for an externally added subscription.
        wait_db = os.path.join(work, "wait.db")
        waiter = Headless(
            [
                "--run-seconds", "60",
                "--db", wait_db,
                "--proxy-port", "0",
                "--batch", "10",
                "--concurrency", "10",
                "--timeout", "1000",
                "--slow-interval", "1",
                "--pool-target", "3",
                "--health-interval", "1",
                "--health-timeout", "700",
                "--health-url", origin.url(),
                "--health-expected", "204",
                "--test-url", origin.url(),
                "--test-expected", "204",
            ]
        )
        check("wait: empty database waits", waiter.wait_for(r"WAIT no subscriptions", timeout=10) >= 0)
        check("wait: subscription inserted", insert_subscription(wait_db, subscription.url()))
        check("wait: fetched and activated", waiter.wait_for(r"READY local=", timeout=30) >= 0)
        wait_port = waiter.proxy_port()
        ok, detail = wait_curl_ok(wait_port, origin.url())
        check("wait: traffic works", ok, str(detail))
        waiter.stop()
    finally:
        if app is not None:
            app.stop()
        for proxy in proxies:
            proxy.stop()
        subscription.close()
        origin.close()

    print()
    print(f"{len(PASSED)} passed, {len(FAILED)} failed")
    if FAILED:
        print("Failed:", ", ".join(FAILED))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
