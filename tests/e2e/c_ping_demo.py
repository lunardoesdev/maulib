#!/usr/bin/env python3
"""Run tests/c/test_ping.c against a local Xray VLESS server.

Builds nothing: expects dist/test_ping and dist/libmilib.so to exist.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

import target_204  # noqa: E402
from run_links import start_xray  # noqa: E402


def main():
    binary = os.path.join(ROOT, "dist", "test_ping")
    if not os.path.exists(binary):
        print(f"SKIP: {binary} not built (run: make test-c-ping)")
        return 2

    workdir = tempfile.mkdtemp(prefix="milib-c-ping-")
    target, _ = target_204.start(0)
    target_port = target.server_address[1]
    url = f"http://127.0.0.1:{target_port}/204"

    xray = start_xray(workdir, 0)
    try:
        print(f"local Xray VLESS: {xray['link']}", file=sys.stderr)
        print(f"target URL:       {url}", file=sys.stderr)
        print("ping (ms):", file=sys.stderr)
        proc = subprocess.run([binary, xray["link"], url])
        return proc.returncode
    finally:
        target.shutdown()
        if xray["proc"].poll() is None:
            xray["proc"].terminate()
            try:
                xray["proc"].wait(timeout=5)
            except subprocess.TimeoutExpired:
                xray["proc"].kill()


if __name__ == "__main__":
    sys.exit(main())
