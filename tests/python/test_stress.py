#!/usr/bin/env python3
"""Add/remove stress test: 100 hot-swap cycles without FD/goroutine leaks."""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from test_api import DEAD_VLESS, LIB_PATH, Mewnet  # noqa: E402

CYCLES = int(os.environ.get("MEWNET_STRESS_CYCLES", "100"))


def fd_count():
    return len(os.listdir("/proc/self/fd"))


class StressTest(unittest.TestCase):
    def test_add_remove_cycles(self):
        if not os.path.exists(LIB_PATH):
            self.skipTest(f"{LIB_PATH} not built")

        api = Mewnet()
        api.init(options={"mixed_port": 0, "log_level": "error"})
        try:
            baseline_status = api.status()
            baseline_fds = fd_count()
            baseline_rss = baseline_status["rss_bytes"]

            for i in range(CYCLES):
                added = api.add(DEAD_VLESS, {"select": False})
                self.assertTrue(added["ok"], f"cycle {i}: {added}")
                name = added["added"][0]["name"]
                removed = api.remove(name)
                self.assertTrue(removed["ok"], f"cycle {i}: {removed}")

            final_status = api.status()
            final_fds = fd_count()
            final_rss = final_status["rss_bytes"]
            listed = api.list()

            print(
                f"\ncycles={CYCLES} "
                f"fds={baseline_fds}->{final_fds} "
                f"goroutines={baseline_status['goroutines']}->{final_status['goroutines']} "
                f"rss={baseline_rss // 1024}KiB->{final_rss // 1024}KiB",
                file=sys.stderr,
            )

            self.assertEqual([p["name"] for p in listed["proxies"]], ["DIRECT"])
            self.assertLessEqual(final_fds, baseline_fds + 16, "file descriptor leak")
            self.assertLessEqual(
                final_status["goroutines"],
                baseline_status["goroutines"] + 32,
                "goroutine leak",
            )
            self.assertLessEqual(
                final_rss - baseline_rss,
                200 * 1024 * 1024,
                "excessive RSS growth",
            )
        finally:
            api.shutdown()


if __name__ == "__main__":
    unittest.main(verbosity=2)
