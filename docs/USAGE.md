# Using mewnet

Practical guide to the library: how to start the core, add and switch proxies,
measure them, and keep a working proxy selected. For the build itself see
[`BUILD.md`](BUILD.md); for the full function list see
[`../libs/mewnet/cmd/mewnet/mewnet.h`](../libs/mewnet/cmd/mewnet/mewnet.h).

Everything below assumes you have:

```text
dist/libmewnet.so
libs/mewnet/cmd/mewnet/mewnet.h          core API
libs/mewnet/cmd/mewnet/mewnet_types.h    structs and enums
libs/mewnet/cmd/mewnet/mewnet_subs.h     subscriptions + discovery agent
```

Link with `-Ldist -lmewnet` and run with `LD_LIBRARY_PATH=dist` (or an rpath).

## Concepts

| Term | What it is |
|---|---|
| **instance** | one running mihomo core; there can be only one per process |
| **proxy** | an outbound server added from a share link, living in group `MEWNET` |
| **link** (`SharedVPNLink`) | one proxy plus its own authenticated SOCKS5 endpoint and per-link tests |
| **task** | a background operation with a callback or a poll handle |
| **subscription** | a URL plus its stored configs (SQLite), grouped and rated |
| **agent** | the background loop that fetches subscriptions, tests configs, selects a node and fails over |

### Two ways to route traffic

Everything runs in one core, but traffic can leave it through two different
doors:

| | Shared inbound | Per-link endpoint |
|---|---|---|
| Listens on | one mixed SOCKS5/HTTP port | one SOCKS5 port per link |
| Proxy used | the selected proxy in `MEWNET` | always the link's own proxy |
| Credentials | none | `mewnet` + generated password |
| Changed by | `mewnet_select_proxy`, `add.select_proxy` | creating/destroying links |

`mewnet_link_create` does not start a second core: it adds the proxy to the same
`MEWNET` group and opens one more listener for it. So the same proxy can serve
the shared inbound (once selected) and its own dedicated port at the same time,
and switching the selection never affects clients connected to a link port.

## C

### Start and stop

```c
#include "mewnet.h"

mewnet_config config = mewnet_config_default();
config.mixed_port = 0;        /* >0 exact port, 0 free port, -1 no inbound */
config.log_level = "warning"; /* NULL keeps the library default */

mewnet_init_result init = mewnet_init(&config);
if (init.instance == NULL) {
    fprintf(stderr, "init failed (%d): %s\n", init.error.code, init.error.message);
    return 1;
}
mewnet_instance *m = init.instance;

/* ... */

mewnet_close(m);              /* stops listeners, cancels pending tasks */
```

Rules that matter:

- one instance per process; a second `mewnet_init` returns
  `MEWNET_ERROR_FAILED` and the message "already initialized";
- the handle stays valid until `mewnet_close`, which also waits for running
  tasks;
- calling `dlclose` on the library is not supported (Go runtime).

`mewnet_get_status(m)` returns a value struct with `running`, `mixed_port`,
`proxies`, `selected`, `goroutines`, `rss_bytes` and `uptime_ms` — useful for a
health endpoint of your own.

### Add, select, remove

```c
mewnet_add_options add = mewnet_add_options_default();
add.name = "work";            /* optional, the fragment is used otherwise */
add.select_proxy = true;      /* make it active right away */

if (mewnet_add_proxy(m, "vless://...", &add) != MEWNET_ERROR_NONE) {
    /* the reason is in the log callback, see "Logging" below */
}

mewnet_select_proxy(m, "work");
mewnet_remove_proxy(m, "work");
```

`mewnet_add_proxy` accepts a single share link, a newline-separated list or a
base64 subscription blob; every parsed entry is added. Names must be unique —
collisions get a numeric suffix. Built-in `DIRECT`, `REJECT`, `GLOBAL` and
`MEWNET` cannot be removed.

### Read state and lists

Lists are allocated by the library and released with the matching `unload`
function. Never `free()` them yourself.

```c
mewnet_proxy_list proxies = mewnet_load_proxies(m);
for (size_t i = 0; i < proxies.count; i++) {
    printf("%-24s %-8s %s:%d %u ms\n",
           proxies.items[i].name, proxies.items[i].proxy_type,
           proxies.items[i].server, proxies.items[i].port,
           proxies.items[i].delay_ms);
}
printf("active: %s\n", proxies.selected);
mewnet_unload_proxies(proxies);
```

| You allocate | You release with |
|---|---|
| `mewnet_instance *` | `mewnet_close()` |
| `mewnet_link *` | `mewnet_link_destroy()` |
| `mewnet_task *` | `mewnet_task_destroy()` |
| `mewnet_load_*()` results | `mewnet_unload_*()` |
| `mewnet_version()` | nothing, static string |

### Measure a proxy

A "ping" is an HTTP request through the tunnel (`URLTest`), not ICMP.

```c
mewnet_delay_options delay = mewnet_delay_options_default();
delay.url = "https://www.gstatic.com/generate_204";
delay.expected = "204";       /* status code that counts as success */
delay.timeout_ms = 5000;

mewnet_delay_result d = mewnet_test_delay(m, "work", &delay);
if (d.ok) {
    printf("%u ms\n", d.delay_ms);
} else {
    printf("failed: %d\n", d.error);
}
```

`mewnet_test_link(m, "vless://...", &delay)` does the same for a raw link
without adding it to the instance.

For a link handle there are three measurements:

```c
mewnet_ping_result ping = mewnet_link_ping(link, &delay);      /* delay_ms  */
mewnet_speed_result speed = mewnet_link_speed(link, &speed_opts); /* mbps   */
mewnet_exit_ip_result ip = mewnet_link_exit_ip(link, &ip_opts);   /* ip     */
```

`speed_opts.bytes` is how many bytes to download (default 5 MiB, `url` points to
any downloadable file). `mewnet_link_exit_ip` asks an echo service which address
it sees and returns it in `ip` with `valid = true`.

### Batch checks

```c
const char *links[] = { link_a, link_b, link_c };
mewnet_batch_test_result batch =
    mewnet_test_links_batch(m, links, 3, &delay, 8 /* concurrency */);

for (size_t i = 0; i < batch.count; i++) {
    printf("%s: ok=%d delay=%u\n", batch.items[i].name,
           batch.items[i].ok, batch.items[i].delay_ms);
}
mewnet_unload_batch_test(batch);
```

`mewnet_test_links_batch_first` cancels the remaining checks as soon as one link
succeeds — the fast path for "I just need any working node". Cancelled items
come back with `MEWNET_ERROR_CANCELLED` and must not be read as failures.

### SharedVPNLink

Use this when another program needs its own SOCKS5 endpoint with credentials,
independent of the mixed inbound.

```c
mewnet_link_options opts = mewnet_link_options_default();
opts.local_proxy = true;      /* false: no SOCKS listener */
opts.password = "gitlab";     /* optional, generated otherwise */

mewnet_link_result created = mewnet_link_create(m, "vless://...", &opts);
if (created.link == NULL) {
    fprintf(stderr, "link: %s\n", created.error.message);
    return 1;
}

mewnet_link_info info = mewnet_link_get_info(created.link);
printf("%s user=%s pass=%s\n",
       info.socks_addr, info.socks_username, info.socks_password);

mewnet_link_destroy(created.link);
```

`info.socks_addr` looks like `127.0.0.1:41237`. Pass it to any SOCKS5 client
with the printed credentials. Proxy-owning links are protected from
`mewnet_remove_proxy`; destroy the link instead.

To make the link the **main** proxy — the one the shared inbound uses — hand
over the handle, no name lookup needed:

```c
mewnet_link_select(created.link);
```

It returns `MEWNET_ERROR_NONE` on success and is exactly equivalent to
`mewnet_select_proxy(m, info.name)`; `mewnet_link_get_info().selected` then
reports `true`. The link's own SOCKS5 port keeps using that same proxy either
way.

### Async

Pass a callback, or `NULL` and poll:

```c
static void on_done(const mewnet_task_result *r, void *user) {
    if (r->state == MEWNET_TASK_DONE && r->error == MEWNET_ERROR_NONE) {
        printf("delay %u ms\n", r->ping.delay_ms);
    }
}

mewnet_task *t = mewnet_link_ping_async(link, &delay, on_done, NULL);
mewnet_task_destroy(t);   /* cancels and waits if still running */
```

```c
mewnet_task *t = mewnet_link_speed_async(link, &speed_opts, NULL, NULL);
mewnet_task_result r;

while ((r = mewnet_task_poll(t)).state == MEWNET_TASK_RUNNING) {
    /* main loop, draw a frame, wait on a socket, ... */
}
if (r.state == MEWNET_TASK_DONE && r.error == MEWNET_ERROR_NONE) {
    printf("%.2f Mbit/s\n", r.speed.mbps);
}
mewnet_task_destroy(t);
```

`r.kind` tells you which result field is filled (`ping`, `speed`, `exit_ip`,
`delay`, or `pings`/`ping_count` for `mewnet_link_ping_many_async`).

The callback runs on an arbitrary OS thread. Do not call `mewnet_close`,
`mewnet_link_destroy` or `mewnet_task_destroy` from inside it, and do not keep
pointers from `r` after the callback returns.

### Streaming HTTP

`mewnet_http_stream` downloads a URL without an instance — handy for fetching a
subscription yourself, with progress and conditional requests:

```c
static int on_chunk(const void *data, size_t len, void *user) {
    fwrite(data, 1, len, (FILE *)user);
    return 0;   /* non-zero stops the transfer */
}

mewnet_http_options ho = { .timeout_ms = 15000, .user_agent = "myapp/1.0" };
mewnet_http_result hr = mewnet_http_stream(url, &ho, on_chunk, file);
printf("%d, %llu bytes, etag=%s\n", hr.status, hr.bytes, hr.etag);
```

The result also carries `profile_title`, `profile_update_interval` and
`subscription_userinfo` parsed from the `#` comments that subscription servers
put in front of the config list.

### Identity and deduplication

```c
mewnet_fingerprint_result fp = mewnet_link_fingerprint(uri);
/* fp.hash: 32 bytes of SHA-256 over protocol + server + port + credentials */
```

Two links for the same server and account hash identically even if the name,
`#fragment` or transport options differ. Subscription storage uses this to
deduplicate; use it too if you keep your own proxy list.

### Logging

```c
static void on_log(mewnet_log_level level, const char *message, void *user) {
    fprintf(stderr, "[%d] %s\n", level, message);
}
mewnet_set_log_level(MEWNET_LOG_INFO);
mewnet_set_log_callback(on_log, NULL);
```

Error codes returned by functions are also mirrored here with a readable
message, which is why the examples do not always print a reason inline.

## Python

The library is a plain C ABI, so `ctypes` works without a wrapper. The test suite
keeps a full set of structure definitions in
[`tests/python/test_api.py`](../tests/python/test_api.py) — copy from there if
you need more structs.

```python
import ctypes

LIB = ctypes.CDLL("./dist/libmewnet.so")

NAME_MAX, ERROR_MAX = 512, 512

class MewnetError(ctypes.Structure):
    _fields_ = [("code", ctypes.c_int), ("message", ctypes.c_char * ERROR_MAX)]

class MewnetConfig(ctypes.Structure):
    _fields_ = [("mixed_port", ctypes.c_int),
                ("home_dir", ctypes.c_char_p),
                ("config_yaml", ctypes.c_char_p),
                ("log_level", ctypes.c_char_p)]

class MewnetInitResult(ctypes.Structure):
    _fields_ = [("instance", ctypes.c_void_p), ("error", MewnetError)]

LIB.mewnet_config_default.restype = MewnetConfig
LIB.mewnet_init.restype = MewnetInitResult
LIB.mewnet_init.argtypes = [ctypes.POINTER(MewnetConfig)]
LIB.mewnet_close.argtypes = [ctypes.c_void_p]

cfg = LIB.mewnet_config_default()
cfg.mixed_port = 18085
cfg.log_level = b"warning"

init = LIB.mewnet_init(ctypes.byref(cfg))
if not init.instance:
    raise RuntimeError(init.error.message.decode())
try:
    ...
finally:
    LIB.mewnet_close(init.instance)
```

Points that bite:

- structures returned by value need `restype`; without it you get a garbage int;
- C strings you pass in may be `bytes`; strings you read back are `bytes` too,
  so `.decode()` them;
- every `mewnet_load_*` result must be released with the matching `unload` call;
- keep a reference to the instance while tasks are alive;
- the library cannot be unloaded, so do not call `dlclose` on it.

## Go

The core is importable without cgo; see
[`../libs/mewnet/README.md`](../libs/mewnet/README.md).

## Subscriptions

Subscriptions and the discovery agent are part of the same library
(`mewnet_subs.h`). SQLite is embedded, so you only pass a database path.

```c
mewnet_subs_config sc = mewnet_subs_config_default();
sc.db_path = "data/subs.db";
/* test_url, test_expected, test_timeout_ms, request_limit,
   request_window_seconds, default_fetch_interval_sec are also here */

mewnet_subs_open_result opened = mewnet_subs_open(&sc);
if (opened.db == NULL) {
    fprintf(stderr, "db: %s\n", opened.error.message);
    return 1;
}
mewnet_subs *db = opened.db;

int64_t group = 0;
mewnet_subs_default_group(db, &group);
mewnet_subs_add_subscription_in_group(db, group, url, 0 /* priority */, NULL);

mewnet_subs_fetch_result fetched = mewnet_subs_fetch(db, subscription_id);
printf("added=%zu updated=%zu removed=%zu\n",
       fetched.added, fetched.updated, fetched.removed);
```

Testing one candidate, with the built-in rate limiter:

```c
mewnet_subs_test_result t = mewnet_subs_test_next(db, m);
if (t.alive) {
    printf("%s alive, %u ms, rating %d -> %d\n",
           t.name, t.delay_ms, t.rating_before, t.rating_after);
}
```

Picking a node to use:

```c
mewnet_subs_proxy chosen;
mewnet_subs_random_best_alive(db, group, 0 /* exclude */, 0 /* avoid */,
                              4 /* candidates */, &chosen);
mewnet_add_options add = mewnet_add_options_default();
add.select_proxy = true;
mewnet_add_proxy(m, chosen.link, &add);
```

`mewnet_subs_test_batch(db, m, group, limit, concurrency)` tests many configs
concurrently and returns the first working one — that is what the fast bootstrap
in the clients uses. `mewnet_subs_close(db)` releases the handle.

### The database

`db_path` is required and points at a plain SQLite file. The library creates it
on first open and migrates the schema in place, so there is no separate setup
step and no `sqlite3` package to install: SQLite is compiled into `libmewnet`.

Every connection opens with `journal_mode=WAL`, `busy_timeout=5000`,
`cache_size=-4096` (4 MB) and `temp_store=FILE`, so a reader and the fetch
worker do not block each other.

| Table | Holds |
|---|---|
| `subscription_groups` | groups; the default `All` group is created automatically |
| `subscriptions` | URL, fetch schedule, ETag/Last-Modified, failure count |
| `proxies` | parsed configs, rating, alive flag, last delay, health signal |
| `proxy_sources` | which subscription last saw a config (generation cleanup) |
| `test_events` | every test result with the rating before and after |

The file is ordinary SQLite, so any tool can read it while your app runs:

```bash
sqlite3 data/subs.db '.tables'
sqlite3 data/subs.db 'SELECT protocol, COUNT(*), SUM(alive) FROM proxies GROUP BY protocol;'
sqlite3 data/subs.db 'SELECT name, rating, last_delay_ms FROM proxies ORDER BY rating DESC LIMIT 5;'
```

Do not write to it from another process while the agent is running — go through
`mewnet_subs_*`. When a background thread of yours needs its own connection,
`mewnet_subs_clone(db)` returns one with the same configuration; release both
with `mewnet_subs_close`.

Full schema and index list: [`SUBSCRIPTIONS.md`](SUBSCRIPTIONS.md#sqlite).

## Discovery agent

The agent is the whole loop: refresh subscriptions on schedule, test configs
within the rate limit, keep a pool of alive proxies, health-check the active one
and fail over.

```c
mewnet_agent_config ac = mewnet_agent_config_default();
ac.db = db;
ac.milib = m;
ac.group_id = group;
ac.proxy_port = mewnet_get_status(m).mixed_port;
ac.batch = 70;
ac.concurrency = 70;
ac.pool_target = 4;

mewnet_agent_callbacks cbs = { on_log, on_state, NULL };
mewnet_agent *agent = mewnet_agent_start(&ac, cbs);

mewnet_agent_request_switch(agent);   /* user pressed "change proxy" */
mewnet_agent_snapshot snap = mewnet_agent_get_snapshot(agent);

mewnet_agent_stop(agent);             /* waits for the thread */
```

`on_state` receives a snapshot with the active proxy, counters (`proxies`,
`alive`, `untested`, `subscriptions`) and `ready`. Both callbacks run on agent
threads; treat them as read-only notifications. `mewnet_agent_stop` frees the
agent, so read the snapshot with `mewnet_agent_get_snapshot` before stopping.

The policy behind the agent (health interval, failure threshold, pool size,
switch order) is documented in [`GTK_RELIABILITY.md`](GTK_RELIABILITY.md).

## Recipes

**Check that traffic really goes through the proxy.** Start the CLI, then:

```bash
curl -sx http://127.0.0.1:18085 https://api.ipify.org; echo
curl -s https://api.ipify.org; echo        # your own address, for comparison
```

Different addresses mean the tunnel is used. `curl -x ... https://example.com`
also proves TLS works end to end.

**Find any working node quickly.** Use `mewnet_test_links_batch_first` with a
generous concurrency (32–70) and a small timeout (3000–5000 ms); it stops at the
first success.

**Keep one selected in a long-running app.** Run `mewnet_agent` and let it own
the selection; do not call `mewnet_select_proxy` yourself while it runs, or the
two will fight.

**Test a list and pick the fastest.** `mewnet_test_links_batch`, sort by
`delay_ms` where `ok` is true, then create a `SharedVPNLink` from the winner.

## Threading rules

- all calls are thread-safe except `mewnet_init` / `mewnet_close`, which must be
  called from one thread;
- async callbacks may arrive on any thread, including while you are calling
  other functions;
- `mewnet_link_destroy` and `mewnet_close` block until their tasks finish, so do
  not call them from a callback or from a thread you cannot block;
- one instance per process, always.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `mewnet_init` returns "already initialized" | one core per process; close the previous instance first |
| segfault at exit / after `dlclose` | the Go runtime cannot be unloaded; never `dlclose` the library |
| delay always fails, but `curl -x` works | check `expected` status and `url`; some servers answer 200, not 204 |
| SOCKS5 per-link endpoint refuses the client | wrong user/password from `mewnet_link_get_info`, or `local_proxy = false` |
| subscription fetch adds nothing | configs are deduplicated within a group by protocol+host+port+credentials |
| traffic stops after switching | long-lived connections stay on the old node; open a new connection |
| process dies with a Go panic | a background goroutine panicked on an untrusted config; isolate it in a separate process |
| `libmewnet.so: cannot open shared object file` | set `LD_LIBRARY_PATH=dist` or add an rpath |
