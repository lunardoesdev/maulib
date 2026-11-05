#!/usr/bin/env python3
"""Multi-proxy e2e test for the SharedVPNLink API.

Creates four proxies in Python through the FFI, routes their traffic through
mihomo, hot-swaps the active proxy while a download is in flight and pings
three proxies concurrently with that download.

Requires an Xray binary (default: /tmp/opencode/xray/xray, override XRAY_BIN).
"""

import ipaddress
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
LIB_PATH = os.environ.get("MILIB_LIB", os.path.join(ROOT, "dist", "libmilib.so"))
XRAY_BIN = os.environ.get("XRAY_BIN", "/tmp/opencode/xray/xray")

sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tests", "python"))

import target_204  # noqa: E402
from test_api import Milib  # noqa: E402

PASSED = []
FAILED = []
PROXIES = 4


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


def count_lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return sum(1 for _ in fh)
    except FileNotFoundError:
        return 0


def curl_mixed(mixed_port, url, timeout=20):
    cmd = [
        "curl",
        "-sS",
        "-o",
        "/dev/null",
        "-w",
        "%{http_code} %{size_download}",
        "--max-time",
        str(timeout),
        "--noproxy",
        "",
        "-x",
        f"http://127.0.0.1:{mixed_port}",
        url,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    parts = proc.stdout.strip().split()
    code = parts[0] if parts else "000"
    size = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
    return proc.returncode, code, size, proc.stderr.strip()


def recv_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("connection closed")
        data += chunk
    return data


def socks5_http(socks_addr, username, password, host, port, path="/204", timeout=10):
    """SOCKS5 user/pass handshake + CONNECT + one HTTP request.

    Returns the HTTP status code, or None when authentication/CONNECT fails.
    """
    host_name, socks_port = socks_addr.rsplit(":", 1)
    with socket.create_connection((host_name, int(socks_port)), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(b"\x05\x01\x02")
        if recv_exact(sock, 2) != b"\x05\x02":
            return None

        user = username.encode()
        pwd = password.encode()
        sock.sendall(b"\x01" + bytes([len(user)]) + user + bytes([len(pwd)]) + pwd)
        if recv_exact(sock, 2) != b"\x01\x00":
            return None

        try:
            ip = ipaddress.ip_address(host)
            atyp = 1 if ip.version == 4 else 4
            addr_bytes = ip.packed
        except ValueError:
            atyp = 3
            addr_bytes = bytes([len(host)]) + host.encode()

        sock.sendall(
            b"\x05\x01\x00"
            + bytes([atyp])
            + addr_bytes
            + int(port).to_bytes(2, "big")
        )
        reply = recv_exact(sock, 4)
        if reply[1] != 0:
            return None
        if reply[3] == 1:
            recv_exact(sock, 4 + 2)
        elif reply[3] == 4:
            recv_exact(sock, 16 + 2)
        else:
            length = recv_exact(sock, 1)[0]
            recv_exact(sock, length + 2)

        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Connection: close\r\n\r\n"
        )
        sock.sendall(request.encode())
        data = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
            if b"\r\n\r\n" in data and len(data) > 64:
                break
        if not data:
            return None
        return int(data.split(b" ", 2)[1])


def start_xray(workdir, index):
    port = free_port()
    user_id = str(uuid.uuid4())
    access = os.path.join(workdir, f"access-{index}.log")
    error = os.path.join(workdir, f"error-{index}.log")
    config = {
        "log": {"loglevel": "warning", "access": access, "error": error},
        "inbounds": [
            {
                "listen": "127.0.0.1",
                "port": port,
                "protocol": "vless",
                "settings": {
                    "clients": [{"id": user_id}],
                    "decryption": "none",
                },
                "streamSettings": {"network": "tcp", "security": "none"},
            }
        ],
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
    path = os.path.join(workdir, f"xray-{index}.json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(config, fh)
    proc = subprocess.Popen(
        [XRAY_BIN, "run", "-c", path],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if not wait_port(port):
        proc.kill()
        raise RuntimeError(f"Xray {index} did not start")
    link = (
        f"vless://{user_id}@127.0.0.1:{port}"
        f"?encryption=none&security=none&type=tcp#xray-{index + 1}"
    )
    return {"proc": proc, "port": port, "access": access, "link": link}


def main():
    if not os.path.exists(XRAY_BIN):
        print(f"SKIP: Xray binary not found at {XRAY_BIN}")
        return 2
    if not os.path.exists(LIB_PATH):
        print(f"SKIP: {LIB_PATH} not built")
        return 2

    workdir = tempfile.mkdtemp(prefix="milib-links-")
    target, _ = target_204.start(0)
    target_port = target.server_address[1]
    base_url = f"http://127.0.0.1:{target_port}"
    xrays = [start_xray(workdir, i) for i in range(PROXIES)]

    api = Milib()
    api.init(os.path.join(workdir, "home"), options={"mixed_port": 0, "log_level": "error"})
    mixed_port = api.status()["mixed_port"]

    links = []
    try:
        for i, xray in enumerate(xrays):
            ptr = api.link_create(xray["link"], {"name": f"xray-{i + 1}"})
            info = api.link_info(ptr)
            links.append({"ptr": ptr, "info": info, "xray": xray})

        check(
            "four links are listed",
            api.link_list()["count"] == PROXIES,
            str(api.link_list()["count"]),
        )

        # 1. Per-link methods.
        for item in links:
            name = item["info"]["name"]
            ptr = item["ptr"]
            ping = api.link_ping(
                ptr, {"url": f"{base_url}/204", "expected": "204", "timeout_ms": 5000}
            )
            check(
                f"{name}: ping through proxy",
                bool(ping and ping.get("ok")) and ping.get("delay", 0) > 0,
                str(ping),
            )

            speed = api.link_speed(
                ptr,
                {"url": f"{base_url}/big?bytes=1048576", "bytes": 1048576, "timeout_ms": 10000},
            )
            check(
                f"{name}: speed through proxy",
                bool(speed and speed.get("ok"))
                and speed.get("bytes") == 1048576
                and speed.get("mbps", 0) > 0,
                str(speed),
            )

            exit_ip = api.link_exit_ip(
                ptr, {"url": f"{base_url}/ip", "timeout_ms": 5000}
            )
            check(
                f"{name}: exit ip through proxy",
                bool(exit_ip and exit_ip.get("ok"))
                and exit_ip.get("ip") == "127.0.0.1"
                and exit_ip.get("valid") is True,
                str(exit_ip),
            )

        # 2. Authenticated per-link SOCKS endpoint.
        sample = links[0]
        socks = sample["info"]["socks"]
        wrong = socks5_http(
            socks["addr"],
            socks["username"],
            "wrong-password",
            "127.0.0.1",
            target_port,
        )
        check("per-link SOCKS rejects wrong password", wrong is None, str(wrong))

        before = count_lines(sample["xray"]["access"])
        code = socks5_http(
            socks["addr"],
            socks["username"],
            socks["password"],
            "127.0.0.1",
            target_port,
        )
        after = count_lines(sample["xray"]["access"])
        check(
            "per-link SOCKS routes through its proxy",
            code == 204 and after > before,
            f"{code} {before}->{after}",
        )

        # 3. Hot-swap while a download is in flight.
        check("select first link", api.select(links[0]["info"]["name"])["ok"], "")
        first_before = count_lines(links[0]["xray"]["access"])
        slow_url = f"{base_url}/slow?bytes=300000&chunks=20&delay=0.25"
        slow_result = {}

        def slow_request():
            slow_result["value"] = curl_mixed(mixed_port, slow_url, timeout=20)

        thread = threading.Thread(target=slow_request, daemon=True)
        thread.start()
        deadline = time.time() + 5
        while time.time() < deadline and count_lines(links[0]["xray"]["access"]) == first_before:
            time.sleep(0.05)
        check("in-flight traffic uses the first link", thread.is_alive(), "download finished too early")

        check("select second link", api.select(links[1]["info"]["name"])["ok"], "")
        second_before = count_lines(links[1]["xray"]["access"])
        thread.join(timeout=30)
        rc, code, size, err = slow_result.get("value", (1, "000", 0, "no result"))
        first_after = count_lines(links[0]["xray"]["access"])
        check(
            "in-flight download survives hot-swap",
            rc == 0 and code == "200" and size == 300000,
            f"{rc} {code} {size} {err}",
        )
        check(
            "in-flight download stayed on the first link",
            first_after > first_before,
            f"{first_before}->{first_after}",
        )

        rc, code, size, err = curl_mixed(mixed_port, f"{base_url}/204")
        second_after = count_lines(links[1]["xray"]["access"])
        check(
            "new traffic goes through the second link",
            rc == 0 and code == "204" and second_after > second_before,
            f"{rc} {code} {second_before}->{second_after} {err}",
        )
        check(
            "first link is idle after the swap",
            count_lines(links[0]["xray"]["access"]) == first_after,
            str(count_lines(links[0]["xray"]["access"])),
        )

        # 4. One download + three concurrent pings.
        check("select first link again", api.select(links[0]["info"]["name"])["ok"], "")
        idle_before = [count_lines(item["xray"]["access"]) for item in links]
        slow_result.clear()
        thread = threading.Thread(target=slow_request, daemon=True)
        thread.start()
        deadline = time.time() + 5
        while time.time() < deadline and count_lines(links[0]["xray"]["access"]) == idle_before[0]:
            time.sleep(0.05)

        ping_links = [item["ptr"] for item in links[1:]]
        started = time.time()
        many = api.links_ping_many(
            ping_links,
            {"url": f"{base_url}/204", "expected": "204", "timeout_ms": 5000},
        )
        ping_seconds = time.time() - started
        still_running = thread.is_alive()
        check(
            "concurrent pings of three links",
            bool(many and many.get("ok"))
            and many.get("count") == 3
            and all(item.get("ok") for item in many["results"]),
            str(many),
        )
        check(
            "pings finish while the download is still running",
            still_running,
            f"download finished first (pings took {ping_seconds:.2f}s)",
        )

        thread.join(timeout=30)
        rc, code, size, err = slow_result.get("value", (1, "000", 0, "no result"))
        check(
            "download completes with all pings in parallel",
            rc == 0 and code == "200" and size == 300000,
            f"{rc} {code} {size} {err}",
        )

        idle_after = [count_lines(item["xray"]["access"]) for item in links]
        check(
            "download used the selected link",
            idle_after[0] > idle_before[0],
            f"{idle_before[0]}->{idle_after[0]}",
        )
        check(
            "each ping used its own link",
            all(idle_after[i] > idle_before[i] for i in range(1, PROXIES)),
            str(idle_after),
        )

        # 5. Async many-ping with a typed C callback.
        async_many = api.links_ping_many_async(
            ping_links,
            {"url": f"{base_url}/204", "expected": "204", "timeout_ms": 5000},
            timeout=15,
        )
        check("async many-ping scheduled", bool(async_many.get("scheduled")), str(async_many))
        check(
            "async many-ping results are ok",
            bool(async_many.get("ok"))
            and all(item.get("ok") for item in async_many.get("results", [])),
            str(async_many),
        )

        many_task = api.links_ping_many_task(
            ping_links,
            {"url": f"{base_url}/204", "expected": "204", "timeout_ms": 5000},
        )
        check("many-ping poll task scheduled", bool(many_task), str(many_task))
        polled = api.task_poll(many_task)
        deadline = time.time() + 15
        while polled["state"] == 0 and time.time() < deadline:
            time.sleep(0.05)
            polled = api.task_poll(many_task)
        check(
            "many-ping poll results are ok",
            bool(polled.get("ok"))
            and all(item.get("ok") for item in polled.get("results", [])),
            str(polled),
        )
        api.task_destroy(many_task)

        # 6. Destroy one link; its running task is cancelled, the others keep working.
        victim = links.pop()
        victim_addr = victim["info"]["socks"]["addr"]

        slow_task = api.link_speed_task(
            victim["ptr"],
            {
                "url": f"{base_url}/slow?bytes=300000&chunks=20&delay=0.25",
                "bytes": 300000,
                "timeout_ms": 20000,
            },
        )
        check("link task scheduled", bool(slow_task), str(slow_task))
        first_poll = api.task_poll(slow_task)
        check("link task is running", first_poll["state"] == 0, str(first_poll))

        check("destroy link", api.link_destroy(victim["ptr"]) == 0, "")
        cancelled = api.task_poll(slow_task)
        check("destroy cancels the link task", cancelled["state"] == 2, str(cancelled))
        api.task_destroy(slow_task)
        check(
            "destroyed link disappears",
            api.link_list()["count"] == PROXIES - 1,
            str(api.link_list()["count"]),
        )
        with self_raises(OSError):
            host, port = victim_addr.rsplit(":", 1)
            socket.create_connection((host, int(port)), timeout=1).close()

        gone = api.links_ping_many(
            [victim["ptr"]],
            {"url": f"{base_url}/204", "expected": "204", "timeout_ms": 1000},
        )
        check(
            "pinging a destroyed link fails",
            bool(gone and gone.get("ok")) and gone["results"][0]["ok"] is False,
            str(gone),
        )
        survivor = api.link_ping(
            links[0]["ptr"],
            {"url": f"{base_url}/204", "expected": "204", "timeout_ms": 5000},
        )
        check(
            "surviving links still work",
            bool(survivor and survivor.get("ok")),
            str(survivor),
        )
    finally:
        api.shutdown()
        target.shutdown()
        for xray in xrays:
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


class self_raises:
    """Tiny context manager to assert that a block raises a given error."""

    def __init__(self, exc_type):
        self.exc_type = exc_type

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        if exc_type is None:
            raise AssertionError(f"expected {self.exc_type.__name__}")
        return issubclass(exc_type, self.exc_type)


if __name__ == "__main__":
    sys.exit(main())
