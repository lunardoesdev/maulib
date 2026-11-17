# mewnet-gtk

C + GTK4 control panel for subscriptions and proxy discovery. The interface is
in English; logs use stable machine-readable event names.

```bash
make gtk
# or
./build/apps/mewnet-gtk/mewnet-gtk \
  --db data/mewnet-gtk.db \
  --proxy-port 18085 \
  https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub
```

GTK4 development files are required (`gtk4.pc`). If GTK4 is not installed,
`make subs` simply skips this binary.

## What it does

- add/remove subscriptions (paged list, no priority);
- fetch a new subscription immediately after it is added;
- show the active proxy: id, name, protocol, address, delay;
- **Change proxy** picks a random node among the best verified proxies
  (highest rating, then lowest delay), excluding the current and previous one;
- show the local mixed proxy, for example `http://127.0.0.1:18085`;
- log discovery events: `FETCH`, `BATCH`, `ACTIVE`, `TEST`, `HEALTH FAIL`,
  `SWITCH`, `DISCOVERED`;
- if there are no subscriptions, wait and start as soon as one appears;
- closing the window stops discovery and the local proxy.

## Reliability model

- The local proxy starts with the application and stays up until exit.
- While there is no working node, untested/dead nodes are checked in batches
  of 70 with first-success cancellation.
- Once a node works, it is selected automatically.
- The active node is health-checked every 6 seconds with a real GET through the local mixed proxy; two failures in the last three checks
  mark it dead and fail over to the next verified node.
- Discovery never retests healthy nodes. It prefers untested nodes, then dead
  ones, and grows a pool of up to `--pool-target` verified proxies.
- If all proxies fail, discovery returns to fast batches and immediately pulls
  the next subscription.
- Subscriptions are refreshed at most once per hour, except for newly added
  subscriptions and emergencies when no working node exists.

The reliability engine lives in `libmewnet` and is shared with the CLI/TUI
front-end. Subscription rows are loaded one page at a time (50 by default).
Full specification and failure matrix:
[`docs/GTK_RELIABILITY.md`](../../docs/GTK_RELIABILITY.md).

## Options

```text
--db PATH              SQLite database (default data/mewnet-gtk.db)
--proxy-port N         local mixed proxy port (0 = free port)
--batch N              fast discovery batch size (default 70)
--concurrency N        concurrent checks (default 70)
--timeout MS           per-proxy timeout (default 6000)
--fetch-interval S     subscription refresh interval (default 3600)
--slow-interval S      delay between checks when a proxy works (default 24)
--pool-target N        verified proxies to keep ready (default 12)
--health-interval S    active proxy health check period (default 6)
--health-timeout MS    active proxy health check timeout (default 2500)
--health-url URL       health check URL (default https://lua.org)
--health-expected CODE expected health status (default 200)
--test-url URL         discovery test URL (default gstatic 204)
--test-expected CODE   discovery expected status (default 204)
--slow-factor N        switch when the active is N x the pool median (default 3, 0 = off)
--slow-floor MS        ... and slower than this absolute latency (default 1000)
--slow-checks N        consecutive slow health checks before switching (default 3)
--slow-cooldown S      minimum gap between slowness switches (default 600)
--screenshot PATH      render the window to PNG and exit (UI smoke test)
```

Positional arguments are subscription URLs added at start.

## Verification

```bash
curl -x http://127.0.0.1:18085 https://lua.org
```

Automated offline failover/recovery test:

```bash
make test-agent
```

It starts local HTTP proxies and a local `204` origin, then verifies bootstrap,
pool growth, random switching across at least three best nodes, health
failover, total outage, recovery, and the "wait for a subscription" path. See `tests/e2e/run_gtk_agent.py`.
