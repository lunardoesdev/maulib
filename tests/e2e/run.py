#!/usr/bin/env python3
"""End-to-end test: mihomo FFI + local Xray VLESS server.

Requires an Xray binary (default: /tmp/opencode/xray/xray, override with
XRAY_BIN). No root and no TUN are used.

Scenario:
  1. add a VLESS link pointing at a local Xray
  2. delay test through the proxy (local 204 target)
  3. delay test against the public generate_204 URL
  4. HTTP request through the mixed inbound, confirmed by the Xray access log
  5. asynchronous delay test with a C callback
  6. remove the proxy: traffic falls back to DIRECT, Xray sees nothing new
  7. stop Xray: delay and traffic fail cleanly
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
LIB_PATH = os.environ.get("MEWNET_LIB", os.path.join(ROOT, "dist", "libmewnet.so"))
XRAY_BIN = os.environ.get("XRAY_BIN", "/tmp/opencode/xray/xray")

sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tests", "python"))
import target_204  # noqa: E402
from test_api import Mewnet  # noqa: E402

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


def wait_port(port, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.2)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.05)
    return False


def curl_through(mixed_port, url, timeout=10):
    cmd = [
        "curl",
        "-sS",
        "-o",
        "/dev/null",
        "-w",
        "%{http_code}",
        "--max-time",
        str(timeout),
        "--noproxy",
        "",
        "-x",
        f"http://127.0.0.1:{mixed_port}",
        url,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.returncode, proc.stdout.strip(), proc.stderr.strip()


class Lib(Mewnet):
    def init(self, home):
        return super().init(
            home=home, options={"mixed_port": 0, "log_level": "warning"}
        )

    def test_link(self, link, url, expected="204", timeout_ms=5000):
        return super().test_link(
            link, {"url": url, "expected": expected, "timeout_ms": timeout_ms}
        )

    def delay(self, name, url, expected="204", timeout_ms=5000):
        return self.test_delay(
            name, {"url": url, "expected": expected, "timeout_ms": timeout_ms}
        )


def main():
    if not os.path.exists(XRAY_BIN):
        print(f"SKIP: Xray binary not found at {XRAY_BIN}")
        return 2
    if not os.path.exists(LIB_PATH):
        print(f"SKIP: {LIB_PATH} not built")
        return 2

    workdir = tempfile.mkdtemp(prefix="mewnet-e2e-")
    access_log = os.path.join(workdir, "access.log")
    error_log = os.path.join(workdir, "error.log")
    home = os.path.join(workdir, "home")

    target, _ = target_204.start(0)
    target_port = target.server_address[1]
    target_url = f"http://127.0.0.1:{target_port}/204"

    xray_port = free_port()
    user_id = str(uuid.uuid4())
    xray_config = {
        "log": {
            "loglevel": "info",
            "access": access_log,
            "error": error_log,
        },
        "inbounds": [
            {
                "listen": "127.0.0.1",
                "port": xray_port,
                "protocol": "vless",
                "settings": {
                    "clients": [{"id": user_id}],
                    "decryption": "none",
                },
                "streamSettings": {"network": "tcp", "security": "none"},
            }
        ],
        # Newer Xray versions block private/loopback targets from VLESS inbounds
        # by default (SSRF protection). Allow loopback explicitly for the test.
        "outbounds": [
            {
                "protocol": "freedom",
                "settings": {
                    "finalRules": [
                        {
                            "action": "allow",
                            "networks": ["tcp"],
                            "ip": ["127.0.0.0/8"],
                        }
                    ]
                },
            }
        ],
    }
    config_path = os.path.join(workdir, "xray.json")
    with open(config_path, "w", encoding="utf-8") as fh:
        json.dump(xray_config, fh)

    xray = subprocess.Popen(
        [XRAY_BIN, "run", "-c", config_path],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if not wait_port(xray_port):
        xray.kill()
        print("FAIL: Xray did not start")
        return 1

    link = (
        f"vless://{user_id}@127.0.0.1:{xray_port}"
        f"?encryption=none&security=none&type=tcp#local-xray"
    )

    api = Lib()
    api.init(home)
    mixed_port = api.status()["mixed_port"]
    check("mixed inbound is listening", wait_port(mixed_port), f"port {mixed_port}")

    try:
        # 1. add
        added = api.add(link)
        check("add VLESS link", bool(added and added.get("ok")), str(added))
        name = added["added"][0]["name"]
        check("fragment becomes proxy name", name == "local-xray", name)

        listed = api.list()
        names = [p["name"] for p in listed["proxies"]]
        check("list contains proxy", name in names, str(names))
        check("proxy auto-selected", listed["selected"] == name, listed["selected"])

        # 2. local delay
        result = api.delay(name, target_url)
        check(
            "delay through VLESS (local 204)",
            bool(result and result.get("ok")) and result.get("delay", 0) > 0,
            str(result),
        )

        # 2b. test a link without adding it
        result = api.test_link(link, target_url)
        check(
            "test_link without adding",
            bool(result and result.get("ok")) and result.get("delay", 0) > 0,
            str(result),
        )

        # 3. public delay
        result = api.delay(
            name, "https://www.gstatic.com/generate_204", timeout_ms=10000
        )
        check(
            "delay through VLESS (gstatic 204)",
            bool(result and result.get("ok")) and result.get("delay", 0) > 0,
            str(result),
        )

        # 4. traffic through the mixed inbound
        before = count_lines(access_log)
        rc, code, err = curl_through(mixed_port, target_url)
        after = count_lines(access_log)
        check("HTTP via mixed inbound returns 204", rc == 0 and code == "204", f"{rc} {code} {err}")
        check("Xray access log shows the request", after > before, f"{before} -> {after}")

        # 5. async delay with a typed callback
        async_result = api.test_delay_async(
            name, {"url": target_url, "expected": "204"}, timeout=10
        )
        check("async task scheduled", bool(async_result.get("scheduled")), str(async_result))
        check(
            "async result ok",
            bool(async_result.get("ok")) and async_result.get("delay", 0) > 0,
            str(async_result),
        )

        # 6. remove -> DIRECT
        before = count_lines(access_log)
        removed = api.remove(name)
        check("remove proxy", bool(removed and removed.get("ok")), str(removed))
        listed = api.list()
        check(
            "selection falls back to DIRECT",
            listed["selected"] == "DIRECT",
            listed["selected"],
        )
        rc, code, err = curl_through(mixed_port, target_url)
        after = count_lines(access_log)
        check("DIRECT traffic still works", rc == 0 and code == "204", f"{rc} {code} {err}")
        check("Xray is no longer used", after == before, f"{before} -> {after}")

        # 7. dead proxy after Xray stops
        added = api.add(link)
        name = added["added"][0]["name"]
        xray.terminate()
        xray.wait(timeout=5)
        result = api.delay(name, target_url, timeout_ms=2000)
        check("delay fails when Xray is down", bool(result and not result.get("ok")), str(result))
        rc, code, err = curl_through(mixed_port, target_url, timeout=5)
        check("traffic fails when Xray is down", rc != 0 or code != "204", f"{rc} {code} {err}")
        api.remove(name)

        # optional real link
        real = os.environ.get("MEWNET_TEST_VLESS")
        if real:
            added = api.add(real)
            check("add real VLESS link", bool(added and added.get("ok")), str(added))
            if added and added.get("ok"):
                real_name = added["added"][0]["name"]
                result = api.delay(real_name, "https://www.gstatic.com/generate_204", timeout_ms=10000)
                check(
                    "real VLESS delay",
                    bool(result and result.get("ok")),
                    str(result),
                )
                api.remove(real_name)
    finally:
        api.shutdown()
        target.shutdown()
        if xray.poll() is None:
            xray.terminate()
            try:
                xray.wait(timeout=5)
            except subprocess.TimeoutExpired:
                xray.kill()

    print()
    print(f"{len(PASSED)} passed, {len(FAILED)} failed")
    if FAILED:
        print("failed:", ", ".join(FAILED))
        print(f"logs: {workdir}")
        return 1
    return 0


def count_lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return sum(1 for _ in fh)
    except FileNotFoundError:
        return 0


if __name__ == "__main__":
    sys.exit(main())
