#!/usr/bin/env python3
"""ABI/unit tests for libmewnet.so via ctypes (v2 API)."""

import ctypes
import os
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
LIB_PATH = os.environ.get("MEWNET_LIB", os.path.join(ROOT, "dist", "libmewnet.so"))

DEAD_VLESS = (
    "vless://11111111-2222-3333-4444-555555555555@127.0.0.1:1"
    "?encryption=none&security=none&type=tcp#dead-node"
)

NAME_MAX = 512
TYPE_MAX = 32
HOST_MAX = 256
ADDR_MAX = 64
AUTH_MAX = 512
URL_MAX = 2048
LINK_MAX = 8192
IP_MAX = 64
ERROR_MAX = 512

MEWNET_ERROR_NONE = 0
MEWNET_ERROR_FAILED = 1
MEWNET_ERROR_INVALID_ARGUMENT = 2
MEWNET_ERROR_NOT_FOUND = 3
MEWNET_ERROR_TIMEOUT = 4
MEWNET_ERROR_CANCELLED = 5
MEWNET_ERROR_CLOSED = 6

MEWNET_TASK_RUNNING = 0
MEWNET_TASK_DONE = 1
MEWNET_TASK_CANCELLED = 2

MEWNET_TASK_DELAY = 1
MEWNET_TASK_PING = 2
MEWNET_TASK_SPEED = 3
MEWNET_TASK_EXIT_IP = 4
MEWNET_TASK_PING_MANY = 5


class MewnetError(ctypes.Structure):
    _fields_ = [("code", ctypes.c_int), ("message", ctypes.c_char * ERROR_MAX)]


class MewnetConfig(ctypes.Structure):
    _fields_ = [
        ("mixed_port", ctypes.c_int),
        ("home_dir", ctypes.c_char_p),
        ("config_yaml", ctypes.c_char_p),
        ("log_level", ctypes.c_char_p),
    ]


class MewnetInitResult(ctypes.Structure):
    _fields_ = [("instance", ctypes.c_void_p), ("error", MewnetError)]


class MewnetStatusInfo(ctypes.Structure):
    _fields_ = [
        ("ok", ctypes.c_bool),
        ("error", ctypes.c_int),
        ("version", ctypes.c_char * TYPE_MAX),
        ("mihomo_version", ctypes.c_char * TYPE_MAX),
        ("mode", ctypes.c_char * TYPE_MAX),
        ("selected", ctypes.c_char * NAME_MAX),
        ("running", ctypes.c_bool),
        ("mixed_port", ctypes.c_int),
        ("socks_port", ctypes.c_int),
        ("http_port", ctypes.c_int),
        ("proxies", ctypes.c_int),
        ("goroutines", ctypes.c_uint),
        ("rss_bytes", ctypes.c_ulonglong),
        ("uptime_ms", ctypes.c_ulonglong),
    ]


class MewnetProxyInfo(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * NAME_MAX),
        ("proxy_type", ctypes.c_char * TYPE_MAX),
        ("server", ctypes.c_char * HOST_MAX),
        ("port", ctypes.c_int),
        ("source", ctypes.c_char * TYPE_MAX),
        ("alive", ctypes.c_bool),
        ("delay_ms", ctypes.c_uint),
    ]


class MewnetProxyList(ctypes.Structure):
    _fields_ = [
        ("ok", ctypes.c_bool),
        ("error", ctypes.c_int),
        ("items", ctypes.POINTER(MewnetProxyInfo)),
        ("count", ctypes.c_size_t),
        ("selected", ctypes.c_char * NAME_MAX),
    ]


class MewnetAddOptions(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("select_proxy", ctypes.c_bool),
        ("skip_cert_verify", ctypes.c_bool),
    ]


class MewnetLinkOptions(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("select_proxy", ctypes.c_bool),
        ("local_proxy", ctypes.c_bool),
        ("username", ctypes.c_char_p),
        ("password", ctypes.c_char_p),
        ("proxy_password", ctypes.c_char_p),
    ]


class MewnetLinkResult(ctypes.Structure):
    _fields_ = [("link", ctypes.c_void_p), ("error", MewnetError)]


class MewnetLinkInfo(ctypes.Structure):
    _fields_ = [
        ("ok", ctypes.c_bool),
        ("error", ctypes.c_int),
        ("id", ctypes.c_uint64),
        ("name", ctypes.c_char * NAME_MAX),
        ("proxy_type", ctypes.c_char * TYPE_MAX),
        ("server", ctypes.c_char * HOST_MAX),
        ("port", ctypes.c_int),
        ("uri", ctypes.c_char * LINK_MAX),
        ("socks_addr", ctypes.c_char * ADDR_MAX),
        ("socks_username", ctypes.c_char * AUTH_MAX),
        ("socks_password", ctypes.c_char * AUTH_MAX),
        ("selected", ctypes.c_bool),
    ]


class MewnetLinkList(ctypes.Structure):
    _fields_ = [
        ("ok", ctypes.c_bool),
        ("error", ctypes.c_int),
        ("items", ctypes.POINTER(MewnetLinkInfo)),
        ("count", ctypes.c_size_t),
    ]


class MewnetDelayOptions(ctypes.Structure):
    _fields_ = [
        ("url", ctypes.c_char_p),
        ("timeout_ms", ctypes.c_int),
        ("expected", ctypes.c_char_p),
    ]


class MewnetDelayResult(ctypes.Structure):
    _fields_ = [
        ("ok", ctypes.c_bool),
        ("error", ctypes.c_int),
        ("name", ctypes.c_char * NAME_MAX),
        ("proxy_type", ctypes.c_char * TYPE_MAX),
        ("server", ctypes.c_char * HOST_MAX),
        ("port", ctypes.c_int),
        ("url", ctypes.c_char * URL_MAX),
        ("delay_ms", ctypes.c_uint),
    ]


class MewnetPingResult(ctypes.Structure):
    _fields_ = [
        ("ok", ctypes.c_bool),
        ("error", ctypes.c_int),
        ("delay_ms", ctypes.c_uint),
    ]


class MewnetSpeedOptions(ctypes.Structure):
    _fields_ = [
        ("url", ctypes.c_char_p),
        ("bytes", ctypes.c_longlong),
        ("timeout_ms", ctypes.c_int),
    ]


class MewnetSpeedResult(ctypes.Structure):
    _fields_ = [
        ("ok", ctypes.c_bool),
        ("error", ctypes.c_int),
        ("status", ctypes.c_int),
        ("bytes", ctypes.c_longlong),
        ("seconds", ctypes.c_double),
        ("bytes_per_second", ctypes.c_double),
        ("mbps", ctypes.c_double),
    ]


class MewnetExitIPOptions(ctypes.Structure):
    _fields_ = [
        ("url", ctypes.c_char_p),
        ("format", ctypes.c_int),
        ("timeout_ms", ctypes.c_int),
    ]


class MewnetExitIPResult(ctypes.Structure):
    _fields_ = [
        ("ok", ctypes.c_bool),
        ("error", ctypes.c_int),
        ("status", ctypes.c_int),
        ("valid", ctypes.c_bool),
        ("ip", ctypes.c_char * IP_MAX),
    ]


class MewnetPingManyResult(ctypes.Structure):
    _fields_ = [
        ("ok", ctypes.c_bool),
        ("error", ctypes.c_int),
        ("items", ctypes.POINTER(MewnetPingResult)),
        ("count", ctypes.c_size_t),
    ]


class MewnetTaskResult(ctypes.Structure):
    _fields_ = [
        ("state", ctypes.c_int),
        ("kind", ctypes.c_int),
        ("error", ctypes.c_int),
        ("delay", MewnetDelayResult),
        ("ping", MewnetPingResult),
        ("speed", MewnetSpeedResult),
        ("exit_ip", MewnetExitIPResult),
        ("pings", ctypes.POINTER(MewnetPingResult)),
        ("ping_count", ctypes.c_size_t),
    ]


MEWNET_TASK_CB = ctypes.CFUNCTYPE(
    None, ctypes.POINTER(MewnetTaskResult), ctypes.c_void_p
)
MEWNET_LOG_CB = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p)


def cstr(buf):
    if buf is None:
        return ""
    if isinstance(buf, bytes):
        return buf.split(b"\0", 1)[0].decode("utf-8", "replace")
    return bytes(buf).split(b"\0", 1)[0].decode("utf-8", "replace")


class Mewnet:
    def __init__(self, path=LIB_PATH):
        self.lib = ctypes.CDLL(path)
        lib = self.lib

        lib.mewnet_abi_version.argtypes = []
        lib.mewnet_abi_version.restype = ctypes.c_int
        lib.mewnet_version.argtypes = []
        lib.mewnet_version.restype = ctypes.c_char_p

        lib.mewnet_config_default.argtypes = []
        lib.mewnet_config_default.restype = MewnetConfig
        lib.mewnet_init.argtypes = [ctypes.POINTER(MewnetConfig)]
        lib.mewnet_init.restype = MewnetInitResult
        lib.mewnet_close.argtypes = [ctypes.c_void_p]
        lib.mewnet_close.restype = None

        lib.mewnet_get_status.argtypes = [ctypes.c_void_p]
        lib.mewnet_get_status.restype = MewnetStatusInfo

        lib.mewnet_load_proxies.argtypes = [ctypes.c_void_p]
        lib.mewnet_load_proxies.restype = MewnetProxyList
        lib.mewnet_unload_proxies.argtypes = [MewnetProxyList]
        lib.mewnet_unload_proxies.restype = None

        lib.mewnet_add_options_default.argtypes = []
        lib.mewnet_add_options_default.restype = MewnetAddOptions
        lib.mewnet_add_proxy.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.POINTER(MewnetAddOptions),
        ]
        lib.mewnet_add_proxy.restype = ctypes.c_int
        lib.mewnet_remove_proxy.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.mewnet_remove_proxy.restype = ctypes.c_int
        lib.mewnet_select_proxy.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.mewnet_select_proxy.restype = ctypes.c_int

        lib.mewnet_delay_options_default.argtypes = []
        lib.mewnet_delay_options_default.restype = MewnetDelayOptions
        lib.mewnet_test_delay.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.POINTER(MewnetDelayOptions),
        ]
        lib.mewnet_test_delay.restype = MewnetDelayResult
        lib.mewnet_test_link.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.POINTER(MewnetDelayOptions),
        ]
        lib.mewnet_test_link.restype = MewnetDelayResult

        lib.mewnet_link_options_default.argtypes = []
        lib.mewnet_link_options_default.restype = MewnetLinkOptions
        lib.mewnet_link_create.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.POINTER(MewnetLinkOptions),
        ]
        lib.mewnet_link_create.restype = MewnetLinkResult
        lib.mewnet_link_destroy.argtypes = [ctypes.c_void_p]
        lib.mewnet_link_destroy.restype = None
        lib.mewnet_link_select.argtypes = [ctypes.c_void_p]
        lib.mewnet_link_select.restype = ctypes.c_int
        lib.mewnet_link_get_info.argtypes = [ctypes.c_void_p]
        lib.mewnet_link_get_info.restype = MewnetLinkInfo

        lib.mewnet_load_links.argtypes = [ctypes.c_void_p]
        lib.mewnet_load_links.restype = MewnetLinkList
        lib.mewnet_unload_links.argtypes = [MewnetLinkList]
        lib.mewnet_unload_links.restype = None

        lib.mewnet_link_ping.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(MewnetDelayOptions),
        ]
        lib.mewnet_link_ping.restype = MewnetPingResult

        lib.mewnet_speed_options_default.argtypes = []
        lib.mewnet_speed_options_default.restype = MewnetSpeedOptions
        lib.mewnet_link_speed.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(MewnetSpeedOptions),
        ]
        lib.mewnet_link_speed.restype = MewnetSpeedResult

        lib.mewnet_exit_ip_options_default.argtypes = []
        lib.mewnet_exit_ip_options_default.restype = MewnetExitIPOptions
        lib.mewnet_link_exit_ip.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(MewnetExitIPOptions),
        ]
        lib.mewnet_link_exit_ip.restype = MewnetExitIPResult

        lib.mewnet_link_ping_many.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.c_size_t,
            ctypes.POINTER(MewnetDelayOptions),
        ]
        lib.mewnet_link_ping_many.restype = MewnetPingManyResult
        lib.mewnet_unload_ping_many.argtypes = [MewnetPingManyResult]
        lib.mewnet_unload_ping_many.restype = None

        lib.mewnet_test_delay_async.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.POINTER(MewnetDelayOptions),
            MEWNET_TASK_CB,
            ctypes.c_void_p,
        ]
        lib.mewnet_test_delay_async.restype = ctypes.c_void_p
        lib.mewnet_link_ping_async.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(MewnetDelayOptions),
            MEWNET_TASK_CB,
            ctypes.c_void_p,
        ]
        lib.mewnet_link_ping_async.restype = ctypes.c_void_p
        lib.mewnet_link_speed_async.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(MewnetSpeedOptions),
            MEWNET_TASK_CB,
            ctypes.c_void_p,
        ]
        lib.mewnet_link_speed_async.restype = ctypes.c_void_p
        lib.mewnet_link_exit_ip_async.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(MewnetExitIPOptions),
            MEWNET_TASK_CB,
            ctypes.c_void_p,
        ]
        lib.mewnet_link_exit_ip_async.restype = ctypes.c_void_p
        lib.mewnet_link_ping_many_async.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.c_size_t,
            ctypes.POINTER(MewnetDelayOptions),
            MEWNET_TASK_CB,
            ctypes.c_void_p,
        ]
        lib.mewnet_link_ping_many_async.restype = ctypes.c_void_p

        lib.mewnet_task_poll.argtypes = [ctypes.c_void_p]
        lib.mewnet_task_poll.restype = MewnetTaskResult
        lib.mewnet_task_cancel.argtypes = [ctypes.c_void_p]
        lib.mewnet_task_cancel.restype = ctypes.c_int
        lib.mewnet_task_destroy.argtypes = [ctypes.c_void_p]
        lib.mewnet_task_destroy.restype = None

        lib.mewnet_set_log_level.argtypes = [ctypes.c_int]
        lib.mewnet_set_log_level.restype = None
        lib.mewnet_set_log_callback.argtypes = [MEWNET_LOG_CB, ctypes.c_void_p]
        lib.mewnet_set_log_callback.restype = None

        self.instance = None

    # -- helpers ---------------------------------------------------------

    @staticmethod
    def _proxy_dict(info):
        return {
            "name": cstr(info.name),
            "type": cstr(info.proxy_type),
            "server": cstr(info.server),
            "port": info.port,
            "source": cstr(info.source),
            "alive": bool(info.alive),
            "delay": info.delay_ms,
        }

    @staticmethod
    def _link_dict(info):
        username = cstr(info.socks_username)
        password = cstr(info.socks_password)
        return {
            "ok": bool(info.ok),
            "error": int(info.error),
            "link_id": info.id,
            "name": cstr(info.name),
            "type": cstr(info.proxy_type),
            "server": cstr(info.server),
            "port": info.port,
            "link": cstr(info.uri),
            "selected": bool(info.selected),
            "socks": {
                "addr": cstr(info.socks_addr),
                "username": username,
                "password": password,
            },
            "auth": {"username": username, "password": password},
        }

    @staticmethod
    def _delay_dict(info):
        return {
            "ok": bool(info.ok),
            "error": int(info.error),
            "name": cstr(info.name),
            "type": cstr(info.proxy_type),
            "server": cstr(info.server),
            "port": info.port,
            "url": cstr(info.url),
            "delay": info.delay_ms,
        }

    @staticmethod
    def _ping_dict(info):
        return {
            "ok": bool(info.ok),
            "error": int(info.error),
            "delay": info.delay_ms,
        }

    @staticmethod
    def _speed_dict(info):
        return {
            "ok": bool(info.ok),
            "error": int(info.error),
            "status": info.status,
            "bytes": info.bytes,
            "seconds": info.seconds,
            "bytes_per_second": info.bytes_per_second,
            "mbps": info.mbps,
        }

    @staticmethod
    def _exit_ip_dict(info):
        return {
            "ok": bool(info.ok),
            "error": int(info.error),
            "status": info.status,
            "valid": bool(info.valid),
            "ip": cstr(info.ip),
        }

    @classmethod
    def _task_dict(cls, result):
        data = {
            "ok": result.state == MEWNET_TASK_DONE and result.error == MEWNET_ERROR_NONE,
            "state": result.state,
            "kind": result.kind,
            "error": int(result.error),
        }
        if result.error != MEWNET_ERROR_NONE:
            return data
        if result.kind == MEWNET_TASK_DELAY:
            data.update(cls._delay_dict(result.delay))
        elif result.kind == MEWNET_TASK_PING:
            data.update(cls._ping_dict(result.ping))
        elif result.kind == MEWNET_TASK_SPEED:
            data.update(cls._speed_dict(result.speed))
        elif result.kind == MEWNET_TASK_EXIT_IP:
            data.update(cls._exit_ip_dict(result.exit_ip))
        elif result.kind == MEWNET_TASK_PING_MANY:
            count = int(result.ping_count)
            results = []
            if result.pings:
                results = [cls._ping_dict(result.pings[i]) for i in range(count)]
            data.update({"count": count, "results": results})
        return data

    @staticmethod
    def _delay_options(options):
        result = MewnetDelayOptions()
        if not options:
            return result
        if options.get("url"):
            result.url = options["url"].encode()
        if options.get("expected"):
            result.expected = str(options["expected"]).encode()
        result.timeout_ms = int(options.get("timeout_ms", 0) or 0)
        return result

    @staticmethod
    def _speed_options(options):
        result = MewnetSpeedOptions()
        if not options:
            return result
        if options.get("url"):
            result.url = options["url"].encode()
        result.bytes = int(options.get("bytes", 0) or 0)
        result.timeout_ms = int(options.get("timeout_ms", 0) or 0)
        return result

    @staticmethod
    def _exit_ip_options(options):
        result = MewnetExitIPOptions()
        if not options:
            return result
        if options.get("url"):
            result.url = options["url"].encode()
        if options.get("format") == "json":
            result.format = 1
        result.timeout_ms = int(options.get("timeout_ms", 0) or 0)
        return result

    def _await_task(self, schedule, timeout=10):
        event = threading.Event()
        box = {}

        def on_done(result, _user):
            box["payload"] = self._task_dict(result.contents)
            event.set()

        cb = MEWNET_TASK_CB(on_done)
        task = schedule(cb)
        if not task:
            return {"ok": False, "error": "task not scheduled"}
        if not event.wait(timeout):
            self.lib.mewnet_task_destroy(task)
            return {"ok": False, "error": "callback timeout"}
        payload = box["payload"]
        payload["scheduled"] = True
        self.lib.mewnet_task_destroy(task)
        return payload

    # -- lifecycle -------------------------------------------------------

    def init(self, home=None, config=None, options=None):
        opts = self.lib.mewnet_config_default()
        if options:
            opts.mixed_port = int(options.get("mixed_port", 0) or 0)
            if options.get("log_level"):
                opts.log_level = options["log_level"].encode()
        if home:
            opts.home_dir = home.encode()
        if config:
            opts.config_yaml = config.encode()
        result = self.lib.mewnet_init(ctypes.byref(opts))
        if not result.instance:
            message = cstr(result.error.message) or f"init failed (code {result.error.code})"
            raise RuntimeError(message)
        self.instance = result.instance
        return result.instance

    def shutdown(self):
        if self.instance:
            self.lib.mewnet_close(self.instance)
            self.instance = None

    # -- proxy management ------------------------------------------------

    def _load_proxies(self):
        return self.lib.mewnet_load_proxies(self.instance)

    def _proxy_names(self):
        proxies = self._load_proxies()
        names = {self._proxy_dict(proxies.items[i])["name"] for i in range(proxies.count)}
        selected = cstr(proxies.selected)
        self.lib.mewnet_unload_proxies(proxies)
        return names, selected

    def add(self, link, options=None):
        before, _ = self._proxy_names()
        opts = self.lib.mewnet_add_options_default()
        if options:
            if options.get("name"):
                opts.name = options["name"].encode()
            if "select" in options:
                opts.select_proxy = bool(options["select"])
            if options.get("skip_cert_verify"):
                opts.skip_cert_verify = True
        rc = self.lib.mewnet_add_proxy(self.instance, link.encode(), ctypes.byref(opts))
        if rc != MEWNET_ERROR_NONE:
            return {"ok": False, "error": f"add proxy failed (code {rc})"}
        proxies = self._load_proxies()
        added = []
        for i in range(proxies.count):
            item = self._proxy_dict(proxies.items[i])
            if item["name"] not in before:
                added.append(item)
        selected = cstr(proxies.selected)
        self.lib.mewnet_unload_proxies(proxies)
        return {"ok": True, "added": added, "selected": selected}

    def remove(self, name):
        rc = self.lib.mewnet_remove_proxy(self.instance, name.encode())
        if rc != MEWNET_ERROR_NONE:
            return {"ok": False, "error": f"remove proxy failed (code {rc})"}
        return {"ok": True, "removed": {"name": name}}

    def list(self):
        proxies = self._load_proxies()
        if not proxies.ok:
            return {"ok": False, "error": f"load proxies failed (code {proxies.error})"}
        result = {
            "ok": True,
            "selected": cstr(proxies.selected),
            "proxies": [self._proxy_dict(proxies.items[i]) for i in range(proxies.count)],
        }
        self.lib.mewnet_unload_proxies(proxies)
        return result

    def select(self, name):
        rc = self.lib.mewnet_select_proxy(self.instance, name.encode())
        if rc != MEWNET_ERROR_NONE:
            return {"ok": False, "error": f"select proxy failed (code {rc})"}
        return {"ok": True, "selected": name}

    def status(self):
        info = self.lib.mewnet_get_status(self.instance)
        return {
            "ok": bool(info.ok),
            "error": int(info.error),
            "version": cstr(info.version),
            "mihomo_version": cstr(info.mihomo_version),
            "running": bool(info.running),
            "mode": cstr(info.mode),
            "mixed_port": info.mixed_port,
            "socks_port": info.socks_port,
            "http_port": info.http_port,
            "proxies": info.proxies,
            "selected": cstr(info.selected),
            "goroutines": info.goroutines,
            "rss_bytes": info.rss_bytes,
            "uptime_ms": info.uptime_ms,
        }

    # -- latency tests ---------------------------------------------------

    def test_delay(self, name, options=None):
        opts = self._delay_options(options)
        result = self.lib.mewnet_test_delay(self.instance, name.encode(), ctypes.byref(opts))
        return self._delay_dict(result)

    def test_link(self, link, options=None):
        opts = self._delay_options(options)
        result = self.lib.mewnet_test_link(self.instance, link.encode(), ctypes.byref(opts))
        return self._delay_dict(result)

    def test_delay_async(self, name, options=None, timeout=10):
        opts = self._delay_options(options)
        return self._await_task(
            lambda cb: self.lib.mewnet_test_delay_async(
                self.instance, name.encode(), ctypes.byref(opts), cb, None
            ),
            timeout,
        )

    # -- SharedVPNLink ---------------------------------------------------

    def link_create(self, link, options=None):
        opts = self.lib.mewnet_link_options_default()
        if options:
            if options.get("name"):
                opts.name = options["name"].encode()
            if "select" in options:
                opts.select_proxy = bool(options["select"])
            if "local_proxy" in options:
                opts.local_proxy = bool(options["local_proxy"])
            for field in ("username", "password", "proxy_password"):
                if options.get(field):
                    setattr(opts, field, options[field].encode())
        result = self.lib.mewnet_link_create(
            self.instance, link.encode(), ctypes.byref(opts)
        )
        if not result.link:
            message = cstr(result.error.message) or f"link create failed (code {result.error.code})"
            raise RuntimeError(message)
        return result.link

    def link_destroy(self, link):
        self.lib.mewnet_link_destroy(link)
        return 0

    def link_info(self, link):
        return self._link_dict(self.lib.mewnet_link_get_info(link))

    def link_select(self, link):
        return int(self.lib.mewnet_link_select(link))

    def link_list(self):
        links = self.lib.mewnet_load_links(self.instance)
        result = {
            "ok": bool(links.ok),
            "error": int(links.error),
            "count": links.count,
            "links": [self._link_dict(links.items[i]) for i in range(links.count)],
        }
        self.lib.mewnet_unload_links(links)
        return result

    def link_ping(self, link, options=None):
        opts = self._delay_options(options)
        return self._ping_dict(self.lib.mewnet_link_ping(link, ctypes.byref(opts)))

    def link_speed(self, link, options=None):
        opts = self._speed_options(options)
        return self._speed_dict(self.lib.mewnet_link_speed(link, ctypes.byref(opts)))

    def link_exit_ip(self, link, options=None):
        opts = self._exit_ip_options(options)
        return self._exit_ip_dict(self.lib.mewnet_link_exit_ip(link, ctypes.byref(opts)))

    def link_ping_async(self, link, options=None, timeout=10):
        opts = self._delay_options(options)
        return self._await_task(
            lambda cb: self.lib.mewnet_link_ping_async(link, ctypes.byref(opts), cb, None),
            timeout,
        )

    def link_speed_async(self, link, options=None, timeout=10):
        opts = self._speed_options(options)
        return self._await_task(
            lambda cb: self.lib.mewnet_link_speed_async(link, ctypes.byref(opts), cb, None),
            timeout,
        )

    def link_exit_ip_async(self, link, options=None, timeout=10):
        opts = self._exit_ip_options(options)
        return self._await_task(
            lambda cb: self.lib.mewnet_link_exit_ip_async(link, ctypes.byref(opts), cb, None),
            timeout,
        )


    def link_ping_task(self, link, options=None):
        opts = self._delay_options(options)
        return self.lib.mewnet_link_ping_async(link, ctypes.byref(opts), MEWNET_TASK_CB(), None)

    def link_speed_task(self, link, options=None):
        opts = self._speed_options(options)
        return self.lib.mewnet_link_speed_async(link, ctypes.byref(opts), MEWNET_TASK_CB(), None)

    def task_poll(self, task):
        return self._task_dict(self.lib.mewnet_task_poll(task))

    def task_cancel(self, task):
        return self.lib.mewnet_task_cancel(task)

    def task_destroy(self, task):
        self.lib.mewnet_task_destroy(task)

    def links_ping_many(self, links, options=None):
        opts = self._delay_options(options)
        handles = (ctypes.c_void_p * len(links))(*[int(link) for link in links])
        result = self.lib.mewnet_link_ping_many(
            self.instance, handles, len(links), ctypes.byref(opts)
        )
        data = {
            "ok": bool(result.ok),
            "error": int(result.error),
            "count": result.count,
            "results": [self._ping_dict(result.items[i]) for i in range(result.count)],
        }
        self.lib.mewnet_unload_ping_many(result)
        return data

    def links_ping_many_async(self, links, options=None, timeout=10):
        opts = self._delay_options(options)
        handles = (ctypes.c_void_p * len(links))(*[int(link) for link in links])
        return self._await_task(
            lambda cb: self.lib.mewnet_link_ping_many_async(
                self.instance, handles, len(links), ctypes.byref(opts), cb, None
            ),
            timeout,
        )

    def links_ping_many_task(self, links, options=None):
        opts = self._delay_options(options)
        handles = (ctypes.c_void_p * len(links))(*[int(link) for link in links])
        return self.lib.mewnet_link_ping_many_async(
            self.instance, handles, len(links), ctypes.byref(opts), MEWNET_TASK_CB(), None
        )


class ApiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(LIB_PATH):
            raise unittest.SkipTest(f"{LIB_PATH} not built")
        cls.api = Mewnet()
        cls.api.init(options={"mixed_port": 0, "log_level": "warning"})

    @classmethod
    def tearDownClass(cls):
        cls.api.shutdown()

    def test_01_version(self):
        self.assertEqual(self.api.lib.mewnet_abi_version(), 2)
        self.assertEqual(self.api.lib.mewnet_version(), b"0.2.0")

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

        fresh = Mewnet()
        fresh.init(options={"mixed_port": 0, "log_level": "warning"})
        self.assertTrue(fresh.status()["ok"])

        # A second live instance in the same process must be rejected.
        second = Mewnet()
        with self.assertRaises(RuntimeError):
            second.init(options={"mixed_port": 0})

        fresh.shutdown()
        type(self).api = Mewnet()
        type(self).api.init(options={"mixed_port": 0, "log_level": "warning"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
