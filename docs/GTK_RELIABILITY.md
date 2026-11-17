# Ultra-reliable `mewnet-gtk`: specification

Goal: at any moment the application must either serve a working proxy on
`127.0.0.1:<port>` or actively and predictably recover. It must not silently
hang in a "proxy exists but does not work" state, and it must not hammer the
network endlessly when everything already works.

## Architecture

Reliability lives in the `libmewnet` library (`src/agent.c`). GTK4 and the
CLI/TUI are thin clients: they open the database and Mihomo, start the agent,
render events, and send `switch` / `fetch subscription` commands.

## Invariants

1. The local mixed proxy listens from startup until the window is closed.
2. If the database holds a live node, the application selects it automatically.
3. The active node is checked separately from discovery. Two failed health
   checks out of the last three mark it dead and trigger an immediate failover.
4. While a working node exists, discovery does not re-check live nodes. It looks
   only at untested ones, then at dead ones.
5. The "change proxy" button picks a random node from the best live ones
   (rating, then latency), excluding the current and the previously selected one.
6. Subscription and group priorities are removed from the behaviour: selection is
   driven by rating, latency, and `health_failures`.
7. Health-check is a signal for selection, not a liveness test: it does not
   set `alive=0`. Otherwise temporary failures would regularly zero the pool.
8. While there are no working nodes, discovery is aggressive: batches of 70,
   stopping at the first success.
9. If all known nodes are dead, the application does not loop on them:
   it re-checks dead nodes and pulls the next subscription at the same time.
10. If there are no subscriptions, the application waits. As soon as a subscription
    appears, it is downloaded and discovery starts automatically.
11. A node that keeps answering but is much slower than the pool is a bad active
    choice even though it never fails: the health-check latency is smoothed and
    compared with the pool median, and the agent switches away.
12. Closing the window stops both discovery and the local proxy.

## States

```text
WAIT_SUBSCRIPTIONS
  no subscriptions -> wait; on addition fetch immediately

FAST_DISCOVERY  (alive == 0)
  batch(70, first-success)
    -> found: ACTIVE, READY, HEALTHY
    -> not found: emergency fetch of the next subscription, retry

HEALTHY  (alive > 0)
  health check of the active every 6 s (2 failures out of 3 -> failover)
  slowness check on the same probe: EWMA vs pool median
    (>= 3x the median and >= 1000 ms for 3 checks in a row -> swap)
  slow discovery: 1 candidate per step
    pool < target  -> step 2 s
    pool >= target -> step 24 s
  subscription fetch: no more than once per hour

FAILOVER
  mark the active one dead
    -> next alive + quick check
    -> check failed: mark dead, take the next
    -> no alive: FAST_DISCOVERY
```

## Default timings

| Parameter | Value | Meaning |
|---|---:|---|
| `--batch` | 70 | fast batch size |
| `--concurrency` | 70 | concurrent checks |
| `--timeout` | 4000 ms | timeout for a single node |
| `--pool-target` | 12 | how many live nodes to keep in reserve |
| `--health-interval` | 6 s | check period for the active node |
| `--health-timeout` | 2500 ms | health-check timeout |
| `--slow-factor` | 3 | switch when the active is this many times the pool median (0 disables) |
| `--slow-floor` | 1000 ms | ... and above this absolute latency |
| `--slow-checks` | 3 | consecutive slow health checks before switching |
| `--slow-cooldown` | 600 s | minimum gap between slowness-driven switches |
| failures before failover | 2 of 3 | protection against false positives and against "flapping" nodes |
| `--slow-interval` | 24 s | discovery step when the pool is full |
| step when pool < target | 2 s | fast growth of the reserve |
| `--fetch-interval` | 3600 s | regular subscription refresh |
| pool-batch cooldown | 10 s | after "change proxy" |
| emergency fetch cooldown | 10 s / 30 s | total failure / switching with no candidates |

The worst-case detection time for a dead active node is roughly
`health_interval + health_timeout * 3` (about 15–20 s including retries).
Slowness needs `health_interval * slow_checks` (about 18 s by default).

The latency comparison mixes two probes: the active node is measured with
`--health-url`, the pool baseline comes from the discovery tests (`--test-url`).
Both are single requests to fast endpoints, and the absolute `--slow-floor`
keeps a systematically slower health URL from triggering; point `--health-url`
at `--test-url` if you want the two numbers strictly comparable.

## What happens on failures

| Situation | Action |
|---|---|
| the active node stopped responding | 2 failures out of the last 3 -> `record_test(false)` -> next alive |
| the active node answers but is slow | EWMA more than `slow_factor` x the pool median and at least `slow_floor` for `slow_checks` checks -> random verified node from the pool |
| the pool has fewer than two measured nodes | no baseline: the slowness check stays quiet, nothing is switched |
| the network itself is slow (every node is slow) | one switch at most per `slow_cooldown`, so the pool does not rotate |
| the active node was removed from the subscription | on the next cycle `current_id=0` -> `first_alive` |
| no live nodes, untested ones remain | `BATCH reason=fast`, first-success |
| no live nodes, no untested ones | `BATCH` re-checks dead ones + `emergency FETCH` |
| the subscription failed to download | log `FETCH FAILED`, retry after a pause; the application does not crash |
| no subscriptions | `WAIT no subscriptions`, polling 0.5 s |
| a new subscription was added | `pending_fetch_id` -> fetch immediately, then discovery |
| "change proxy", other alive nodes exist | random from the best (top-5 by rating/latency), excluding the current and the previous one |
| "change proxy", no other alive nodes | batch of candidates; if empty — emergency fetch |
| pool < target after switching | one pool-batch (does not change the active node) |
| window closed | stop worker -> `mewnet_close` -> port released |

## Why it does not waste network

- Health-check is a light request to `generate_204` (or a local URL).
- Live nodes are not tested in discovery: only by health-check.
- Slow mode is one candidate every 24 s when the pool is full.
- Subscriptions: no more than once an hour; a new subscription and an emergency
  are the exceptions.
- Batches of 70 start only when there are no live nodes or the user explicitly
  asks for a new proxy.

## Memory

- The UI holds one page of subscriptions (50 rows), not the whole list.
- The agent uses SQL counters and `ORDER BY ... LIMIT 1`, without loading
  millions of rows.
- Full proxy lists and links are not loaded into the UI.
- The log is capped at 2000 lines; older ones are trimmed.
- SQLite page cache is limited to 4 MB per connection, `temp_store=FILE`, WAL auto-checkpoint.
- Measurement: 30 subscriptions and 12 638 proxies — GTK ~159 MB RSS; 10 030
  subscriptions — ~159 MB, that is, memory does not grow with the number of subscriptions.
- A subscription is downloaded to a temporary file, and the SQLite transaction
  starts only after the download: a slow source does not block health-check or
  writing results.

## Observability

The journal and stdout carry stable events:

```text
SUBS n=...                 number of subscriptions
FETCH ...                  subscription refresh result
BATCH reason=fast|switch|pool tested=... alive=...
ACTIVE id=... name=...     selected node
READY local=http://...     local proxy is ready
HEALTH FAIL try=1/2        the active node did not respond
SLOW try=1/3 ewma=...      the active node answers but is much slower than the pool
DISCOVERED id=...          a new live node was added to the pool
SWITCH requested           the user pressed the button
TEST ...                   candidate check
WAIT no subscriptions ...  waiting for sources
```

## Automated verification

### 1. Regular suite

```bash
make test
```

Covers the Go core, the C ABI, SQLite/rating, and e2e with a local Xray.

### 2. Agent fault tolerance

```bash
make test-agent
```

The test is fully offline: it brings up a local HTTP origin `204` and several
local HTTP proxies, serves them as a subscription, and runs the CLI in
`--headless` mode. It checks 24 assertions:

- bootstrap: `READY`, local port, `204` traffic;
- the pool grew to at least 2 live nodes;
- SIGUSR1 (the "change proxy" button) switches to a different node, and traffic
  keeps working;
- several switches in a row select at least 3 different best nodes,
  not two;
- the active node is identified from the log;
- stopping the active proxy produces `HEALTH FAIL` and an automatic failover;
- traffic works after the failover;
- stopping all proxies triggers `BATCH reason=fast`, and the process stays alive;
- a new working node in the subscription is discovered and activated, and traffic
  works again;
- an empty database waits for a subscription (`WAIT no subscriptions`);
- an external insertion of a subscription is picked up, `READY` happens, and traffic
  works;
- a proxy with artificial latency (450 ms against ~5 ms neighbours) is active at
  bootstrap, produces `SLOW try=...`, is replaced by a fast node, and traffic
  keeps working.

### 3. UI smoke

```bash
./build/apps/mewnet-gtk/mewnet-gtk --db /tmp/ui.db --screenshot /tmp/ui.png
```

The application builds the window, captures a PNG, and exits. This checks that the UI
builds, renders, and shuts down cleanly without a manual click.

## Deduplication

For each proxy an identity is computed as SHA-256 of the protocol, host, port, and
credentials (UUID/password/method+password). The name, `#fragment`, and transport
options do not affect it. The `proxies` table has a unique index on
`(group_id, identity_hash)`. When an old database is opened, identityHash
is backfilled in batches of 200 rows, duplicates are merged, and their sources
are moved to the surviving record.

## Manual check

```bash
make gtk
curl -x http://127.0.0.1:<port> https://lua.org
```

The port is shown in the top bar. The **Change proxy** button switches to the
next live node; when the pool is short, additional candidates are fetched.
