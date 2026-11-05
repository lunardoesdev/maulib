#!/usr/bin/env python3
"""SharedVPNLink API tests that do not require a working upstream proxy.

They cover handle lifecycle, generated/manual auth, the per-link authenticated
SOCKS5 endpoint, error paths, concurrent ping of several links and destroy.
"""

import os
import socket
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from test_api import DEAD_VLESS, LIB_PATH, Milib  # noqa: E402


def recv_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("connection closed")
        data += chunk
    return data


def socks5_auth(addr, username, password, timeout=3.0):
    """Perform a SOCKS5 user/password handshake. Returns True when accepted."""
    host, port = addr.rsplit(":", 1)
    with socket.create_connection((host, int(port)), timeout=timeout) as sock:
        sock.sendall(b"\x05\x01\x02")
        ver, method = recv_exact(sock, 2)
        if ver != 5 or method != 2:
            return False
        user = username.encode()
        pwd = password.encode()
        sock.sendall(b"\x01" + bytes([len(user)]) + user + bytes([len(pwd)]) + pwd)
        _, status = recv_exact(sock, 2)
        return status == 0


class LinkTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(LIB_PATH):
            raise unittest.SkipTest(f"{LIB_PATH} not built")
        cls.api = Milib()
        cls.api.init(options={"mixed_port": 0, "log_level": "error"})

    @classmethod
    def tearDownClass(cls):
        cls.api.shutdown()

    def make_link(self, options=None):
        link = self.api.link_create(DEAD_VLESS, options)
        self.addCleanup(self.api.link_destroy, link)
        return link

    def test_01_create_and_info(self):
        link = self.make_link()
        info = self.api.link_info(link)
        self.assertTrue(info["ok"], info)
        self.assertGreater(info["link_id"], 0)
        self.assertEqual(info["name"], "dead-node")
        self.assertEqual(info["type"], "VLESS")
        self.assertEqual(info["socks"]["username"], "milib")
        self.assertGreaterEqual(len(info["socks"]["password"]), 16)
        self.assertTrue(info["socks"]["addr"].startswith("127.0.0.1:"))

        listed = self.api.link_list()
        self.assertTrue(listed["ok"], listed)
        self.assertIn(info["link_id"], [item["link_id"] for item in listed["links"]])

    def test_02_manual_auth(self):
        link = self.make_link({"username": "user-1", "password": "manual-pass"})
        info = self.api.link_info(link)
        self.assertEqual(info["auth"]["username"], "user-1")
        self.assertEqual(info["auth"]["password"], "manual-pass")
        self.assertTrue(socks5_auth(info["socks"]["addr"], "user-1", "manual-pass"))
        self.assertFalse(socks5_auth(info["socks"]["addr"], "user-1", "wrong"))

    def test_03_generated_auth_is_enforced(self):
        link = self.make_link()
        info = self.api.link_info(link)
        addr = info["socks"]["addr"]
        self.assertTrue(socks5_auth(addr, info["auth"]["username"], info["auth"]["password"]))
        self.assertFalse(socks5_auth(addr, info["auth"]["username"], "wrong-password"))
        self.assertFalse(socks5_auth(addr, "other", info["auth"]["password"]))

    def test_04_local_proxy_can_be_disabled(self):
        link = self.make_link({"local_proxy": False})
        info = self.api.link_info(link)
        self.assertEqual(info["socks"]["addr"], "")

    def test_05_proxy_password_override(self):
        link = self.make_link({"proxy_password": "override-uuid"})
        info = self.api.link_info(link)
        self.assertTrue(info["ok"], info)

    def test_06_ping_dead_link_fails(self):
        link = self.make_link()
        result = self.api.link_ping(
            link,
            {"url": "http://127.0.0.1:9/204", "timeout_ms": 1500, "expected": "204"},
        )
        self.assertFalse(result["ok"], result)

    def test_07_speed_dead_link_fails(self):
        link = self.make_link()
        result = self.api.link_speed(
            link, {"url": "http://127.0.0.1:9/", "timeout_ms": 1500, "bytes": 1024}
        )
        self.assertFalse(result["ok"], result)

    def test_08_exit_ip_dead_link_fails(self):
        link = self.make_link()
        result = self.api.link_exit_ip(
            link, {"url": "http://127.0.0.1:9/ip", "timeout_ms": 1500}
        )
        self.assertFalse(result["ok"], result)

    def test_09_many_pings_are_isolated(self):
        links = [self.make_link() for _ in range(3)]
        result = self.api.links_ping_many(
            links,
            {"url": "http://127.0.0.1:9/204", "timeout_ms": 1200, "expected": "204"},
        )
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["count"], 3)
        self.assertEqual(len(result["results"]), 3)
        for item in result["results"]:
            self.assertFalse(item["ok"], item)
            self.assertIn("error", item)

    def test_10_async_ping_callback(self):
        link = self.make_link()
        payload = self.api.link_ping_async(
            link, {"url": "http://127.0.0.1:9/204", "timeout_ms": 1000}, timeout=10
        )
        self.assertFalse(payload["ok"], payload)
        self.assertNotEqual(payload["error"], 0)

    def test_11_remove_proxy_is_guarded(self):
        link = self.make_link()
        info = self.api.link_info(link)
        result = self.api.remove(info["name"])
        self.assertFalse(result["ok"], result)

    def test_13_async_speed_and_exit_ip(self):
        link = self.make_link()
        speed = self.api.link_speed_async(
            link,
            {"url": "http://127.0.0.1:9/", "timeout_ms": 1200, "bytes": 1024},
            timeout=10,
        )
        self.assertFalse(speed["ok"], speed)
        exit_ip = self.api.link_exit_ip_async(
            link, {"url": "http://127.0.0.1:9/ip", "timeout_ms": 1200}, timeout=10
        )
        self.assertFalse(exit_ip["ok"], exit_ip)

    def test_14_task_poll_and_destroy(self):
        link = self.make_link()
        task = self.api.link_ping_task(
            link, {"url": "http://127.0.0.1:9/204", "timeout_ms": 800}
        )
        self.assertTrue(task)
        deadline = time.time() + 5
        payload = self.api.task_poll(task)
        while payload["state"] == 0 and time.time() < deadline:
            time.sleep(0.02)
            payload = self.api.task_poll(task)
        self.assertNotEqual(payload["state"], 0, payload)
        self.assertFalse(payload["ok"], payload)
        self.assertNotEqual(payload["error"], 0, payload)
        self.api.task_destroy(task)

    def test_12_destroy(self):
        link = self.api.link_create(DEAD_VLESS)
        info = self.api.link_info(link)
        addr = info["socks"]["addr"]
        self.assertEqual(self.api.link_destroy(link), 0)

        # The handle is freed: never touch it again. Check the registry instead.
        self.assertNotIn(
            info["link_id"],
            [item["link_id"] for item in self.api.link_list()["links"]],
        )
        with self.assertRaises(OSError):
            host, port = addr.rsplit(":", 1)
            socket.create_connection((host, int(port)), timeout=1).close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
