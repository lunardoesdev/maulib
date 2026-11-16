# Subscriptions in `libmewnet`

The C part of subscriptions, SQLite and the discovery agent ship in a single `libmewnet`:

- downloads a subscription as a stream, without loading the whole body into memory;
- supports plain text and base64 (standard and URL-safe);
- parses `vless://`, `vmess://`, `trojan://`, `ss://`, `hysteria2://`, ...
- writes configs, rating and test history to SQLite;
- deletes stale configs via the generation mechanism;
- checks configs via `mewnet_test_link_info`;
- respects the shared request limit.

## Build

```bash
make deps          # mise install: go, odin, android-sdk, clang, cmake
make build         # dist/libmewnet.so
make subs          # dist/libmewnet.so + build/apps/mewnet-subs/mewnet-subs
make test-subs     # ctest
```

CMake uses:

- `mewnet` from `dist/libmewnet.so`;
- the SQLite amalgamation inside `libs/mewnet/cmd/mewnet` (public domain);
- clang from mise.

## CLI

```bash
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db sub add URL 100 name
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db sub ls
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db sub rm 1
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db sub priority 1 200

./build/apps/mewnet-subs/mewnet-subs --db data/subs.db --limit 0 fetch 1
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db --limit 0 fetch --due
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db --limit 0 test
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db --limit 10 --window 240 run

./build/apps/mewnet-subs/mewnet-subs --db data/subs.db proxies --limit 20
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db proxy add 1 'vless://...'
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db proxy rm 42
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db proxy show 42
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db stats
```

`run` — the main loop:

1. if there is a due subscription, download the next one by time/failures;
2. otherwise test the next candidate;
3. every external request goes through the shared rate limiter;
4. the test result and rating are written to SQLite.

## Seed subscriptions

`subscriptions/seed.txt`:

```text
100 https://raw.githubusercontent.com/barry-far/V2ray-Config/main/All_Configs_base64_Sub.txt
90  https://raw.githubusercontent.com/ALIILAPRO/v2rayNG-Config/main/server.txt
80  https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub
```

Load the seed into the DB:

```bash
./subscriptions/seed.sh data/subs.db
```

Verified manually:

- a large base64 stream ~2.9 MB;
- plain text with `#` metadata;
- a small base64 subscription.

Public subscriptions are untrusted: configs may be malformed or malicious.
Run the tester as a separate process where possible.

## Formats

Plain:

```text
#profile-title: base64:...
#profile-update-interval: 1
#subscription-userinfo: upload=0; download=123; total=456; expire=789
vless://...
vmess://...
```

Base64:

```text
<base64 of the same text, may contain line breaks and omit padding>
```

The mode is detected from the first 512 bytes:

- `#` or `://` → plain;
- base64 alphabet only → base64.

BOM, CRLF, spaces, `+`/`-`, `/`/`_`, `=` and a missing `=` are supported.

## Subscription groups

Subscriptions are combined into groups. On the first open of the DB a group
`All` is created, and every new subscription goes into it. That is the single
large group holding all subscriptions.

```c
mewnet_subs_group_list groups = mewnet_subs_load_groups(db);
mewnet_subs_add_subscription_in_group(db, group_id, url, priority, name);
```

Available for GUI/CLI:

```bash
mewnet-subs --db data/subs.db groups
mewnet-subs --db data/subs.db group add NAME [priority]
mewnet-subs --db data/subs.db group rm ID
mewnet-subs --db data/subs.db sub add URL 100 name
```

## Config deduplication

Configs are deduplicated within a group by `SHA-256` of the full link
**without the `#name` fragment**.

- the same config from two subscriptions of one group = one `proxies` row;
- the same config with different `#name` = one row;
- a changed password/UUID/transport = a different link = a new row;
- there is no deduplication by host:port.

The config ↔ subscription relation is stored in `proxy_sources`. If a config is
present in several subscriptions, it stays as long as at least one source
exists.

## SQLite

```sql
subscription_groups(
  id, name, priority, enabled, created_at
)

subscriptions(
  id, group_id, url, name, priority, enabled,
  fetch_interval_sec, next_fetch_at, last_fetch_at, last_status,
  failure_count, etag, last_modified, userinfo, last_error,
  generation, created_at
)

proxies(
  id, group_id, link, link_hash, name, protocol, server, port,
  rating, alive, manual, last_tested_at, last_delay_ms, last_error,
  last_up_day, last_down_day,
  UNIQUE(group_id, link_hash)
)

proxy_sources(
  proxy_id, subscription_id, last_seen_generation
)

test_events(
  id, proxy_id, ts, ok, delay_ms, error,
  rating_before, rating_after, rating_delta
)
```

WAL, `busy_timeout`, `foreign_keys` and `ON DELETE CASCADE` are enabled.

## Rating

On success:

- `rating += 1`, but at most once per UTC day;
- `last_up_day = YYYYMMDD`.

On failure:

- `rating -= 1`, but at most once per day;
- `last_down_day = YYYYMMDD`.

The limits are independent: in one day a proxy can get both `+1` and `-1`.

## Candidate selection

Candidates are selected **randomly, weighted by proxy rating**:

```text
weight = 1 + max(0, rating + 10)

key = random_uniform ^ (1 / weight)
ORDER BY key DESC
```

That is, higher-rated proxies come up more often, but the order is not fixed:
configs of equal weight are interleaved. Group and subscription priorities do
not participate.
This is used both in batch discovery and in regular `mewnet_subs_test_next`.

In a fast batch untested entries come first (`last_tested_at IS NULL`), then
dead ones (`alive=0`). So when no live nodes remain, the app does not get stuck
on "no untested left": dead ones are re-tested and the next subscription is
fetched on an emergency basis.

## Rate limit

By default:

- 10 requests;
- a 240 second window;
- an interval of ~24 seconds with jitter.

The limit is shared between fetch and test. Configured with:

```bash
--limit 10 --window 240
--limit 0          # disable (for debugging)
```

## Streaming ingest

```text
mewnet_http_stream
  → sniff plain/base64
  → streaming base64 decoder / passthrough
  → bounded line buffer
  → # metadata → subscriptions
  → link → SHA-256 → proxies UPSERT
  → DELETE stale by generation
```

Memory is bounded:

- sniff: 512 bytes;
- line: 16 KiB;
- SQLite page cache.

The subscription body and the full list of links are never held in memory.

## Discovery CLI: `mewnet-watch`

```bash
./build/apps/mewnet-watch/mewnet-watch --db data/watch.db --proxy-port 18085 \
  --batch 70 --concurrency 70 \
  https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub
```

Behavior:

- creates SQLite;
- adds subscriptions to `All`;
- while there is no alive node, tests untested configs in batches of 70
  concurrently;
- on finding an alive node, adds it to `MILIB`, selects it and prints
  `PROXY http://127.0.0.1:<port>` + `READY`;
- after that discovery goes through the standard rate limiter;
- updates at most one subscription per hour; if there are no untested nodes and
  no working ones, it updates the next one immediately.

Library APIs for this:

- `mewnet_test_links_batch` / `mewnet_test_links_batch_first` /
  `mewnet_unload_batch_test` in `libmewnet`;
- `mewnet_subs_test_batch` — select untested entries, test them as a batch,
  stop at the first alive one (the remaining checks are cancelled), write the
  results and return the first alive one;
- `mewnet_subs_count_untested`, `mewnet_subs_count_alive`,
  `mewnet_subs_first_alive`;
- `mewnet_subs_next_alive` — the next live node for the "change proxy"
  button;
- `mewnet_subs_fetch_now` — fetch without waiting for the rate limiter, for
  emergency updates;
- `mewnet_subs_test_next_now` — a test without the blocking rate limiter;
  pacing is set by the caller;
- `mewnet_subs_test_candidate_now(... scope)` — select a candidate in the given
  scope: `ANY`, `UNTESTED` or `NOT_ALIVE`; used by the agent so it does not
  re-test live nodes and grows the pool;
- `mewnet_subs_load_subscriptions_page`, `mewnet_subs_count_subscriptions`,
  `mewnet_subs_max_last_fetch`, `mewnet_subs_next_subscription_id` — working
  with subscriptions without loading the whole table;
- `mewnet_subs_random_best_alive` — a random one of the best live nodes;
- `mewnet_agent_start` / `stop` / `request_switch` /
  `request_subscription` / `get_snapshot` — a shared discovery agent for GTK
  and CLI/TUI.

## GTK4 control panel: `mewnet-gtk`

```bash
make gtk
./build/apps/mewnet-gtk/mewnet-gtk --db data/mewnet-gtk.db --proxy-port 18085 \
  https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub
```

Window in C + GTK4:

- a paged subscription list, add and remove;
- a "Change proxy" button — a random node from the best alive ones (rating,
  delay), excluding the current and the previous one;
- additionally a CLI/TUI `apps/mewnet-cli` with the same agent;
- current proxy: name, protocol, address, delay;
- log of `FETCH` / `BATCH` / `ACTIVE` / `TEST` / `SWITCH`;
- a line with the address of the local mixed proxy;
- closing the window stops discovery and the proxy.

While there is no alive node, the GUI tests batches of 70 and stops at the first
working one. After activation:

- a health-check of the active node every 6 seconds; two failures out of the
  last three trigger failover;
- discovery does not touch live nodes, it tests untested ones, then dead ones;
- with fewer than 4 live nodes the discovery step is 2 seconds, with a full
  pool 24 seconds;
- subscriptions are updated at most once per hour; a new subscription is
  downloaded immediately;
- if no live nodes remain, batches of 70 and emergency fetch are enabled again.

Full specification in [`docs/GTK_RELIABILITY.md`](GTK_RELIABILITY.md).

## C API

```c
mewnet_subs_config cfg = mewnet_subs_config_default();
cfg.db_path = "data/subs.db";
mewnet_subs_open_result opened = mewnet_subs_open(&cfg);

mewnet_error err = mewnet_subs_add_subscription(opened.db, url, 0, "name");
mewnet_subs_fetch_result fetched = mewnet_subs_fetch_due(opened.db);

mewnet_instance* m = ...; /* mewnet_init with mixed_port = -1 */
mewnet_subs_test_result tested = mewnet_subs_test_next(opened.db, m);

mewnet_subs_close(opened.db);
```

Added for the GUI:

- `mewnet_subs_load_proxy_summaries` — a page without the full `link`;
- `mewnet_subs_get_proxy` — one full record by id;
- `mewnet_subs_add_proxy` — a manual proxy (`manual = 1`);
- `mewnet_subs_remove_proxy`;
- `mewnet_subs_count_proxies`.

Manual proxies are not removed by generation cleanup when they are absent from
a subscription: `DELETE ... AND manual = 0`. If the link appears in a
subscription again, it becomes managed (`manual = 0`).

The full list is in `subscriptions/include/mewnet_subs.h`.
