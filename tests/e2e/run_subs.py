#!/usr/bin/env python3
"""End-to-end test for libmewnet + mewnet-subs CLI.

Starts a local Xray VLESS server and a local HTTP subscription server, then
exercises streaming fetch, base64/plain parsing, testing, rating and SQLite.
"""

import base64
import http.server
import os
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BIN = os.path.join(ROOT, "build", "apps", "mewnet-subs", "mewnet-subs")
XRAY_BIN = os.environ.get("XRAY_BIN", "/tmp/opencode/xray/xray")

sys.path.insert(0, HERE)
import target_204  # noqa: E402
from run_links import start_xray  # noqa: E402

PASSED = []
FAILED = []


def check(name, condition, detail=""):
    if condition:
        PASSED.append(name)
        print(f"PASS  {name}")
    else:
        FAILED.append(name)
        print(f"FAIL  {name}  {detail}")


def run(db, *args):
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = os.pathsep.join(
        [os.path.join(ROOT, "dist"), env.get("LD_LIBRARY_PATH", "")]
    )
    proc = subprocess.run(
        [BIN, "--db", db, "--limit", "0", "--timeout", "5000", *args],
        capture_output=True,
        text=True,
        env=env,
    )
    if proc.returncode != 0:
        print(proc.stdout, file=sys.stderr)
        print(proc.stderr, file=sys.stderr)
    return proc


def serve(directory):
    handler = lambda *a, **kw: http.server.SimpleHTTPRequestHandler(
        *a, directory=directory, **kw
    )
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


def main():
    if not os.path.exists(BIN):
        print(f"SKIP: {BIN} not built (run: make -C apps subs)")
        return 2
    if not os.path.exists(XRAY_BIN):
        print(f"SKIP: Xray binary not found at {XRAY_BIN}")
        return 2

    workdir = tempfile.mkdtemp(prefix="mewnet-subs-e2e-")
    target, _ = target_204.start(0)
    target_port = target.server_address[1]
    xray = start_xray(workdir, 0)

    base_link = xray["link"].split("#", 1)[0]
    live_link = base_link + "#xray-live"
    renamed_link = base_link + "#xray-renamed"
    changed_password = base_link.replace("@", "x@", 1) + "#xray-changed"
    plain = (
        "#profile-title: e2e-plain\n"
        "#profile-update-interval: 1\n"
        f"{live_link}\n"
        f"{renamed_link}\n"
        f"{changed_password}\n"
        "vless://11111111-2222-3333-4444-555555555555@127.0.0.1:1"
        "?encryption=none&security=none&type=tcp#dead-one\n"
    )
    with open(os.path.join(workdir, "plain.txt"), "w", encoding="utf-8") as fh:
        fh.write(plain)
    with open(os.path.join(workdir, "base64.txt"), "w", encoding="utf-8") as fh:
        fh.write(base64.b64encode(plain.encode()).decode() + "\n")

    server, _ = serve(workdir)
    port = server.server_address[1]
    db = os.path.join(workdir, "subs.db")

    try:
        check("add plain", run(db, "sub", "add",
                               f"http://127.0.0.1:{port}/plain.txt", "100", "plain").returncode == 0)
        check("add base64", run(db, "sub", "add",
                                f"http://127.0.0.1:{port}/base64.txt", "90", "b64").returncode == 0)
        check("fetch plain", run(db, "fetch", "1").returncode == 0)
        check("fetch base64", run(db, "fetch", "2").returncode == 0)

        conn = sqlite3.connect(db)
        conn.row_factory = sqlite3.Row
        proxies = conn.execute("SELECT COUNT(*) FROM proxies").fetchone()[0]
        check("duplicates deduped, password change kept", proxies == 3, str(proxies))
        check("default group exists",
              conn.execute("SELECT COUNT(*) FROM subscription_groups WHERE name='All'").fetchone()[0] == 1)
        check("groups command", run(db, "groups").returncode == 0, "")

        xray_row = conn.execute(
            "SELECT id, name FROM proxies WHERE name = 'xray-live' LIMIT 1"
        ).fetchone()
        dead_row = conn.execute(
            "SELECT id FROM proxies WHERE name = 'dead-one' LIMIT 1"
        ).fetchone()
        check("xray config stored", xray_row is not None, "")
        check("dead config stored", dead_row is not None, "")
        changed_row = conn.execute(
            "SELECT id FROM proxies WHERE name = 'xray-changed' LIMIT 1"
        ).fetchone()
        check("password change creates new config", changed_row is not None, "")

        manual_link = (
            "vless://aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee@127.0.0.1:9"
            "?encryption=none&security=none&type=tcp#manual-e2e"
        )
        check("add manual proxy", run(db, "proxy", "add", "1", manual_link).returncode == 0)
        # Force a real 200 (not 304) to exercise generation cleanup.
        updated_path = os.path.join(workdir, "plain.txt")
        with open(updated_path, "w", encoding="utf-8") as fh:
            fh.write(plain + "# refreshed\n")
        future = time.time() + 5
        os.utime(updated_path, (future, future))
        run(db, "fetch", "1")
        manual_row = conn.execute(
            "SELECT id, manual FROM proxies WHERE name = 'manual-e2e' LIMIT 1"
        ).fetchone()
        check("manual proxy survives fetch", manual_row is not None and manual_row["manual"] == 1,
              dict(manual_row) if manual_row else "missing")
        if manual_row:
            check("remove manual proxy",
                  run(db, "proxy", "rm", str(manual_row["id"])).returncode == 0)

        if xray_row:
            proc = run(db, "test", str(xray_row["id"]))
            check("live proxy test", proc.returncode == 0, proc.stderr)
            row = conn.execute(
                "SELECT alive, rating, last_delay_ms, protocol, server, port FROM proxies WHERE id=?",
                (xray_row["id"],),
            ).fetchone()
            check("live proxy alive", row["alive"] == 1, dict(row))
            check("live proxy rating +1", row["rating"] == 1, dict(row))
            check("live proxy metadata", row["protocol"] == "vless" and
                  row["server"] == "127.0.0.1" and row["port"] == xray["port"], dict(row))

        if dead_row:
            run(db, "test", str(dead_row["id"]))
            first = conn.execute("SELECT rating FROM proxies WHERE id=?",
                                 (dead_row["id"],)).fetchone()[0]
            run(db, "test", str(dead_row["id"]))
            second = conn.execute("SELECT rating FROM proxies WHERE id=?",
                                  (dead_row["id"],)).fetchone()[0]
            check("dead proxy rating -1", first == -1, str(first))
            check("dead proxy daily cap", second == -1, str(second))

        events = conn.execute("SELECT COUNT(*) FROM test_events").fetchone()[0]
        check("test events recorded", events >= 3, str(events))
        conn.close()
    finally:
        server.shutdown()
        target.shutdown()
        if xray["proc"].poll() is None:
            xray["proc"].terminate()
            try:
                xray["proc"].wait(timeout=5)
            except subprocess.TimeoutExpired:
                xray["proc"].kill()

    print()
    print(f"{len(PASSED)} passed, {len(FAILED)} failed")
    if FAILED:
        print("failed:", ", ".join(FAILED))
        print(f"logs: {workdir}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
