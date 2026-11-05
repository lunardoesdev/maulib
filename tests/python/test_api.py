#!/usr/bin/env python3
"""ABI/unit tests for libmilib.so via ctypes.

These tests need no Xray: they exercise lifecycle, parsing, validation and
error paths. For the end-to-end proxy test see tests/e2e/run.py.
"""

import ctypes
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
LIB_PATH = os.environ.get("MILIB_LIB", os.path.join(ROOT, "dist", "libmilib.so"))

DEAD_VLESS = (
    "vless://11111111-2222-3333-4444-555555555555@127.0.0.1:1"
    "?encryption=none&security=none&type=tcp#dead-node"
)


class Milib:
    def __init__(self, path=LIB_PATH):
        self.lib = ctypes.CDLL(path)
        self.lib.milib_version.restype = ctypes.c_char_p
        self.lib.milib_free_string.argtypes = [ctypes.c_void_p]
        self.lib.milib_free_string.restype = None

        self.lib.milib_init.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_uint64),
            ctypes.POINTER(ctypes.c_char_p),
        ]
        self.lib.milib_init.restype = ctypes.c_int

        for name in (
            "milib_add_proxy",
            "milib_remove_proxy",
            "milib_list_proxies",
            "milib_select",
            "milib_status",
            "milib_test_delay",
            "milib_test_link",
            "milib_task_poll",
        ):
            fn = getattr(self.lib, name)
            fn.restype = ctypes.c_void_p

        self.lib.milib_shutdown.argtypes = [ctypes.c_uint64]
        self.lib.milib_shutdown.restype = None

        self.lib.milib_link_create.argtypes = [
            ctypes.c_uint64,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_char_p),
        ]
        self.lib.milib_link_create.restype = ctypes.c_void_p
        self.lib.milib_link_destroy.argtypes = [ctypes.c_void_p]
        self.lib.milib_link_destroy.restype = ctypes.c_int
        self.lib.milib_link_list.argtypes = [ctypes.c_uint64]
        self.lib.milib_link_list.restype = ctypes.c_void_p
        for name in (
            "milib_link_info",
            "milib_link_ping",
            "milib_link_speed",
            "milib_link_exit_ip",
        ):
            fn = getattr(self.lib, name)
            fn.argtypes = [ctypes.c_void_p]
            fn.restype = ctypes.c_void_p
        self.lib.milib_links_ping_many.argtypes = [
            ctypes.c_uint64,
            ctypes.c_char_p,
            ctypes.c_char_p,
        ]
        self.lib.milib_links_ping_many.restype = ctypes.c_void_p
        self.lib.milib_links_ping_many_async.argtypes = [
            ctypes.c_uint64,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
        ]
        self.lib.milib_links_ping_many_async.restype = ctypes.c_int64

        self.handle = ctypes.c_uint64(0)

    def _take(self, ptr):
        if not ptr:
            return None
        try:
            raw = ctypes.cast(ptr, ctypes.c_char_p).value
            return json.loads(raw.decode("utf-8")) if raw else None
        finally:
            self.lib.milib_free_string(ptr)

    def init(self, home=None, config=None, options=None):
        err = ctypes.c_char_p()
        rc = self.lib.milib_init(
            home.encode() if home else None,
            config.encode() if config else None,
            json.dumps(options).encode() if options else None,
            ctypes.byref(self.handle),
            ctypes.byref(err),
        )
        if rc != 0:
            message = err.value.decode() if err.value else "unknown error"
            if err.value:
                self.lib.milib_free_string(ctypes.cast(err, ctypes.c_void_p))
            raise RuntimeError(message)
        return self.handle.value

    def add(self, link, options=None):
        return self._take(
            self.lib.milib_add_proxy(
                self.handle,
                link.encode(),
                json.dumps(options).encode() if options else None,
            )
        )

    def remove(self, name):
        return self._take(self.lib.milib_remove_proxy(self.handle, name.encode()))

    def list(self):
        return self._take(self.lib.milib_list_proxies(self.handle))

    def select(self, name, group=None):
        return self._take(
            self.lib.milib_select(
                self.handle,
                group.encode() if group else None,
                name.encode(),
            )
        )

    def status(self):
        return self._take(self.lib.milib_status(self.handle))

    def test_delay(self, name, options=None):
        return self._take(
            self.lib.milib_test_delay(
                self.handle,
                name.encode(),
                json.dumps(options).encode() if options else None,
            )
        )

    def test_link(self, link, options=None):
        return self._take(
            self.lib.milib_test_link(
                self.handle,
                link.encode(),
                json.dumps(options).encode() if options else None,
            )
        )

    def shutdown(self):
        self.lib.milib_shutdown(self.handle)

    def link_create(self, link, options=None):
        err = ctypes.c_char_p()
        ptr = self.lib.milib_link_create(
            self.handle,
            link.encode(),
            json.dumps(options).encode() if options else None,
            ctypes.byref(err),
        )
        if not ptr:
            message = err.value.decode() if err.value else "unknown error"
            if err.value:
                self.lib.milib_free_string(ctypes.cast(err, ctypes.c_void_p))
            raise RuntimeError(message)
        return ptr

    def link_destroy(self, link):
        return self.lib.milib_link_destroy(link)

    def link_info(self, link):
        return self._take(self.lib.milib_link_info(link))

    def link_list(self):
        return self._take(self.lib.milib_link_list(self.handle))

    def link_ping(self, link, options=None):
        return self._take(
            self.lib.milib_link_ping(
                link, json.dumps(options).encode() if options else None
            )
        )

    def link_speed(self, link, options=None):
        return self._take(
            self.lib.milib_link_speed(
                link, json.dumps(options).encode() if options else None
            )
        )

    def link_exit_ip(self, link, options=None):
        return self._take(
            self.lib.milib_link_exit_ip(
                link, json.dumps(options).encode() if options else None
            )
        )

    def links_ping_many(self, link_ids, options=None):
        return self._take(
            self.lib.milib_links_ping_many(
                self.handle,
                json.dumps(list(link_ids)).encode(),
                json.dumps(options).encode() if options else None,
            )
        )

    def links_ping_many_async(self, link_ids, options, cb, user=None):
        return self.lib.milib_links_ping_many_async(
            self.handle,
            json.dumps(list(link_ids)).encode(),
            json.dumps(options).encode() if options else None,
            cb,
            user,
        )


class ApiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(LIB_PATH):
            raise unittest.SkipTest(f"{LIB_PATH} not built")
        cls.api = Milib()
        cls.api.init(options={"mixed_port": 0, "log_level": "warning"})

    @classmethod
    def tearDownClass(cls):
        cls.api.shutdown()

    def test_01_version(self):
        self.assertEqual(self.api.lib.milib_version(), b"0.1.0")

    def test_02_status(self):
        status = self.api.status()
        self.assertTrue(status["ok"], status)
        self.assertTrue(status["running"])
        self.assertGreater(status["mixed_port"], 0)

    def test_03_invalid_link(self):
        result = self.api.add("definitely-not-a-link")
        self.assertFalse(result["ok"])
        self.assertIn("error", result)

    def test_04_add_list_remove(self):
        added = self.api.add(DEAD_VLESS)
        self.assertTrue(added["ok"], added)
        self.assertEqual(len(added["added"]), 1)
        name = added["added"][0]["name"]
        self.assertEqual(name, "dead-node")

        listed = self.api.list()
        names = [p["name"] for p in listed["proxies"]]
        self.assertIn(name, names)
        self.assertEqual(listed["selected"], name)

        removed = self.api.remove(name)
        self.assertTrue(removed["ok"], removed)
        self.assertEqual(removed["removed"]["name"], name)

        listed = self.api.list()
        self.assertNotIn(name, [p["name"] for p in listed["proxies"]])
        self.assertEqual(listed["selected"], "DIRECT")

    def test_05_remove_missing_and_builtin(self):
        self.assertFalse(self.api.remove("does-not-exist")["ok"])
        self.assertFalse(self.api.remove("DIRECT")["ok"])

    def test_06_duplicate_names(self):
        first = self.api.add(DEAD_VLESS, {"select": False})
        second = self.api.add(DEAD_VLESS, {"select": False})
        self.assertTrue(first["ok"], first)
        self.assertTrue(second["ok"], second)
        self.assertEqual(second["added"][0]["name"], "dead-node-2")
        self.api.remove(first["added"][0]["name"])
        self.api.remove(second["added"][0]["name"])

    def test_07_select(self):
        added = self.api.add(DEAD_VLESS)
        name = added["added"][0]["name"]
        self.assertTrue(self.api.select("DIRECT")["ok"])
        self.assertEqual(self.api.list()["selected"], "DIRECT")
        self.assertTrue(self.api.select(name)["ok"])
        self.assertEqual(self.api.list()["selected"], name)
        self.api.remove(name)

    def test_08_dead_proxy_delay_fails(self):
        added = self.api.add(DEAD_VLESS)
        name = added["added"][0]["name"]
        result = self.api.test_delay(
            name,
            {
                "url": "http://127.0.0.1:9/204",
                "timeout_ms": 1500,
                "expected": "204",
            },
        )
        self.assertFalse(result["ok"], result)
        self.api.remove(name)

    def test_09_test_link_invalid(self):
        result = self.api.test_link("nope")
        self.assertFalse(result["ok"])

    def test_10_reinit_after_shutdown(self):
        self.api.shutdown()

        fresh = Milib()
        fresh.init(options={"mixed_port": 0, "log_level": "warning"})
        self.assertTrue(fresh.status()["ok"])

        # A second live instance in the same process must be rejected.
        second = Milib()
        with self.assertRaises(RuntimeError):
            second.init(options={"mixed_port": 0})

        fresh.shutdown()
        type(self).api = Milib()
        type(self).api.init(options={"mixed_port": 0, "log_level": "warning"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
