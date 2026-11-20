# mewnet-cli

CLI/TUI front-end for the shared `libmewnet` discovery agent. No extra
dependencies: POSIX terminal + ANSI, the same engine as the GTK4 app.

```bash
make -C apps cli          # from the repository root
./build/apps/mewnet-cli/mewnet-cli --db data/mewnet-cli.db --proxy-port 18085
```

The TUI shows:

```text
mewnet agent
local proxy : http://127.0.0.1:18085
active      : id=1234  some-node-name
              vless 1.2.3.4:443 84 ms
pool        : subscriptions=30 proxies=12638 alive=4 untested=12500
state       : ready

[s] switch proxy   [q] quit
```

Keys:

- `s` — switch to a random one of the best verified proxies;
- `q` — quit.

Headless mode for scripts/tests prints machine-readable lines:

```bash
./build/apps/mewnet-cli/mewnet-cli --headless --db data/mewnet-cli.db \
  --proxy-port 18085 --run-seconds 60
```

It prints `PROXY http://127.0.0.1:<port>`, `STATE ...` and agent events
(`FETCH`, `BATCH`, `ACTIVE`, `HEALTH FAIL`, `SLOW`, `SWITCH`, `DISCOVERED`).

The agent drops an active proxy that fails health checks (`--health-*`) and also
one that keeps answering but is far slower than the pool: `--slow-factor`
(default 3) times the pool median, above `--slow-floor` (default 1000 ms), for
`--slow-checks` checks in a row (default 3), with at most one such switch per
`--slow-cooldown` seconds (default 600). `--slow-factor 0` disables it.

All reliability behavior (health checks, failover, pool growth, subscription
scheduling) is in the library, so GTK and CLI behave identically. See
[`docs/GTK_RELIABILITY.md`](../../docs/GTK_RELIABILITY.md).
