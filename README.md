<img src="assets/icon.svg" width="64" height="64" alt="">

# mewnet

Run the [mihomo](https://github.com/MetaCubeX/mihomo) proxy core **inside your
own process**. No daemon, no control port, no JSON: a C ABI with plain
functions, value structs and opaque handles that you can call from C, Python,
Odin or anything else with FFI.

You get a local proxy on `127.0.0.1`, per-proxy measurements, subscription
storage in SQLite and a discovery agent that keeps a working node selected — all
inside your binary.

## How it fits together

There is **one core instance per process**. Inside that core:

- a **mixed SOCKS5/HTTP inbound** listens on `127.0.0.1:<port>`;
- a selector group called **`MEWNET`** holds every proxy you add;
- the rules end with `MATCH,MEWNET`, so all traffic that enters the inbound goes
  to whichever proxy is currently selected.

```text
your client ──► 127.0.0.1:18085 (mixed inbound)
                        │
                 [ MEWNET selector ]
                        │
        ┌───────────────┼───────────────┐
   proxy A (selected)  proxy B        proxy C
```

On top of that there are **two independent ways to route traffic**. This is the
part that usually confuses people, so here is the whole model in one table:

| | Shared inbound | Per-link endpoint (SharedVPNLink) |
|---|---|---|
| What listens | one mixed port for the whole process | one SOCKS5 port per link |
| Which proxy is used | the currently **selected** proxy in `MEWNET` | **exactly the proxy of that link**, always |
| Credentials | none | user `mewnet` + generated password (overridable) |
| How you change the exit | `mewnet_select_proxy` / `add.select_proxy` | create another link, or destroy this one |
| Lifetime | until `mewnet_close` | until `mewnet_link_destroy` |
| Created by | `mewnet_init` | `mewnet_link_create` |
| Typical use | one proxy for the whole app | give one thread/process/subscriber its own exit |

Two things that follow from this:

1. **A link is not a second tunnel.** `mewnet_link_create` adds its proxy to the
   same `MEWNET` group, so the proxy can be used both by the shared inbound (if
   you select it) and by its own SOCKS5 port.
2. **Per-link traffic ignores the selection.** If the app switches to proxy C,
   a client connected to link B's SOCKS5 port keeps exiting through B.

## Quick start

Build the core and the clients, give the CLI a subscription, and use the proxy:

```bash
make deps            # pinned toolchain via mise (go, clang, cmake, odin, android-sdk)
make subs            # dist/libmewnet.so + CLI / watch / subs
./build/apps/mewnet-cli/mewnet-cli --headless \
  --db data/mewnet-cli.db --proxy-port 18085 \
  https://example.com/subscription.txt
```

The CLI is the core plus subscriptions plus the agent, so it prints the mixed
port as soon as a node works:

```text
DB data/mewnet-cli.db
PROXY http://127.0.0.1:18085
ACTIVE id=6 name=some-node delay=525
READY local=http://127.0.0.1:18085
```

```bash
curl -x http://127.0.0.1:18085 https://example.com
curl -x http://127.0.0.1:18085 https://api.ipify.org    # exit IP
```

## Route traffic through a proxy

### Path 1 — shared inbound: traffic goes through the selected proxy

```c
mewnet_add_options add = mewnet_add_options_default();
add.select_proxy = true;                 /* make it the selected proxy */
mewnet_add_proxy(m, "vless://...", &add);

int port = mewnet_get_status(m).mixed_port;
printf("use http://127.0.0.1:%d\n", port);
```

Now everything pointed at `http://127.0.0.1:<port>` (or `socks5h://`, the same
port speaks SOCKS5) exits through that proxy:

```bash
curl -x http://127.0.0.1:$PORT https://example.com
```

Switching later is a hot swap: `mewnet_select_proxy(m, "other-node")`. New
connections use the new node; connections that are already open stay on the old
one until they close. If the proxy came from `mewnet_link_create`, use
`mewnet_link_select(link)` and skip the name lookup — see Path 2.

### Path 2 — per-link endpoint: traffic goes through one specific link

Use this when a consumer needs its own exit and its own credentials, without
caring what the rest of the process has selected.

```c
mewnet_link_options opts = mewnet_link_options_default();
opts.local_proxy = true;                 /* default: open a SOCKS5 port */

mewnet_link_result created = mewnet_link_create(m, "vless://...", &opts);
if (created.link == NULL) { /* created.error.message */ }

mewnet_link_info info = mewnet_link_get_info(created.link);
printf("%s %s %s\n", info.socks_addr, info.socks_username, info.socks_password);
```

That is all — traffic sent to `info.socks_addr` with those credentials always
exits through this link:

```bash
curl --socks5-hostname 127.0.0.1:<link_port> \
     --proxy-user mewnet:<generated-password> https://example.com
```

`mewnet_link_ping`, `mewnet_link_speed` and `mewnet_link_exit_ip` also measure
this link specifically, regardless of which proxy is selected.

To make the link the **main** proxy as well — the one the shared inbound uses —
pass the handle instead of looking the name up:

```c
mewnet_link_select(created.link);   /* same as mewnet_select_proxy(m, info.name) */
```

After that, `http://127.0.0.1:<mixed_port>` and the link's own SOCKS5 port both
exit through this node. Selecting something else later moves only the shared
inbound; the link's port always keeps its own proxy.

### Which one do I need?

| Case | Use |
|---|---|
| Whole application goes through one proxy | shared inbound + selection |
| Different subscribers/threads must exit through different proxies | one link per subscriber |
| You want to hand a ready-made SOCKS5 endpoint to another program | per-link endpoint |
| You only need to test links and pick the fastest | links + `mewnet_link_ping_many`, no routing needed |
| Subscription-driven, failover must happen automatically | the agent with the shared inbound |

## C: complete example, shared inbound

This is the whole loop for "my app should send traffic through one proxy": create
the link, hand the handle to `mewnet_link_select`, use the mixed port.

```c
#include <stdio.h>
#include "mewnet.h"

int main(void) {
    mewnet_config config = mewnet_config_default();
    config.mixed_port = 0;           /* 0: pick a free port, -1: no inbound */
    config.log_level = "warning";

    mewnet_init_result init = mewnet_init(&config);
    if (init.instance == NULL) {
        fprintf(stderr, "init failed (%d): %s\n", init.error.code, init.error.message);
        return 1;
    }
    mewnet_instance *m = init.instance;

    /* A link is the proxy plus a handle for tests, selection and destroy. */
    mewnet_link_options opts = mewnet_link_options_default();
    opts.local_proxy = false;        /* the shared mixed port is enough here */

    mewnet_link_result created = mewnet_link_create(m, "vless://...", &opts);
    if (created.link == NULL) {
        fprintf(stderr, "link: %s\n", created.error.message);
        mewnet_close(m);
        return 1;
    }
    mewnet_link *link = created.link;

    /* The handle selects the main proxy; no name lookup anywhere. */
    if (mewnet_link_select(link) != MEWNET_ERROR_NONE) {
        fprintf(stderr, "select failed, see the log callback\n");
        mewnet_link_destroy(link);
        mewnet_close(m);
        return 1;
    }

    mewnet_link_info info = mewnet_link_get_info(link);
    mewnet_status_info status = mewnet_get_status(m);
    printf("proxy: http://127.0.0.1:%d\n", status.mixed_port);
    printf("selected: %s (%s %s:%d)\n",
           status.selected, info.proxy_type, info.server, info.port);

    mewnet_delay_options delay = mewnet_delay_options_default();
    delay.url = "https://www.gstatic.com/generate_204";
    delay.expected = "204";
    delay.timeout_ms = 5000;

    mewnet_ping_result ping = mewnet_link_ping(link, &delay);
    if (ping.ok) {
        printf("delay: %u ms\n", ping.delay_ms);
    }

    /* ... traffic goes through http://127.0.0.1:<mixed_port> ... */

    mewnet_link_destroy(link);
    mewnet_close(m);
    return 0;
}
```

If you have no need for link handles (testing, per-link SOCKS, selection by
handle), a raw link plus `mewnet_add_proxy` does the same in one call:

```c
mewnet_add_options add = mewnet_add_options_default();
add.select_proxy = true;
mewnet_add_proxy(m, "vless://...", &add);
```

Compile:

```bash
cc app.c -Ilibs/mewnet/cmd/mewnet -Ldist -lmewnet -Wl,-rpath,"$PWD/dist" -o app
./app
```

Buffer sizes, list ownership and the rest of the API are in
[`docs/USAGE.md`](docs/USAGE.md) and the header comments.

## C: complete example, one link with its own SOCKS5

No shared inbound and no selection here: the core starts without a mixed port
and the link gets a dedicated authenticated endpoint.

```c
#include <stdio.h>
#include "mewnet.h"

int main(void) {
    mewnet_config config = mewnet_config_default();
    config.mixed_port = -1;          /* no shared inbound at all */

    mewnet_init_result init = mewnet_init(&config);
    if (init.instance == NULL) {
        fprintf(stderr, "init: %s\n", init.error.message);
        return 1;
    }
    mewnet_instance *m = init.instance;

    mewnet_link_options opts = mewnet_link_options_default();
    opts.local_proxy = true;

    mewnet_link_result created = mewnet_link_create(m, "vless://...", &opts);
    if (created.link == NULL) {
        fprintf(stderr, "link: %s\n", created.error.message);
        mewnet_close(m);
        return 1;
    }

    mewnet_link_info info = mewnet_link_get_info(created.link);
    printf("curl --socks5-hostname %s --proxy-user %s:%s https://example.com\n",
           info.socks_addr, info.socks_username, info.socks_password);

    mewnet_delay_options delay = mewnet_delay_options_default();
    mewnet_ping_result ping = mewnet_link_ping(created.link, &delay);
    printf("delay: %u ms\n", ping.delay_ms);

    mewnet_link_destroy(created.link);
    mewnet_close(m);
    return 0;
}
```

`local_proxy = false` skips the SOCKS5 listener when you only need the
measurements. Edge cases, auth rules and hot-swap details:
[`docs/SHARED_VPN_LINK.md`](docs/SHARED_VPN_LINK.md).

## Subscriptions and the agent

A subscription is a URL that returns share links. `libmewnet` downloads it,
stores configs in a SQLite file you choose (`--db data/subs.db`, created and
migrated automatically), tests them under a shared rate limit, rates them
`+1/-1`, and the agent keeps a pool of working nodes and switches the selected
proxy when the active one dies.

```bash
make subs
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db sub add URL 0 name
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db run --limit 10 --window 240
```

The shortest path from a subscription to working traffic is the CLI from Quick
start: it opens the database, starts the core, starts the agent and prints the
mixed port. In your own program the same pieces are `mewnet_subs_*` plus
`mewnet_agent_start`; see [`docs/USAGE.md`](docs/USAGE.md) and
[`docs/SUBSCRIPTIONS.md`](docs/SUBSCRIPTIONS.md).

## C API: subscriptions, storage and the agent

Everything here is declared in
[`mewnet_subs.h`](libs/mewnet/cmd/mewnet/mewnet_subs.h) and ships inside
`libmewnet`. SQLite is compiled in (FTS5 included): `mewnet_subs_open` creates
the file, migrates the schema and sets up WAL, so there is no separate storage
library and no SQL to run at install time.

The examples below are complete programs. Each one compiles with:

```bash
cc example.c -Ilibs/mewnet/cmd/mewnet -Ldist -lmewnet \
   -Wl,-rpath,"$PWD/dist" -o example
```

### Opening a database and filling it from a subscription

`mewnet_subs_config_default()` gives you a working configuration; only `db_path`
is required. `mewnet_subs_open` returns a handle and an error, and every later
call reports its own result, so failures never arrive as exceptions or globals.

```c
/* Add one subscription, fetch it, report what landed in the database. */
#include <stdio.h>
#include "mewnet_subs.h"

int main(void) {
    /* Defaults: test URL generate_204, 5 s timeout, 10 requests per 240 s,
       6 h refresh interval. Override only what you need. */
    mewnet_subs_config config = mewnet_subs_config_default();
    config.db_path = "data/subs.db";   /* created and migrated on first open */
    config.request_limit = 0;          /* this example makes a single request */

    mewnet_subs_open_result opened = mewnet_subs_open(&config);
    if (opened.db == NULL) {
        fprintf(stderr, "open: %s\n", opened.error.message);
        return 1;
    }
    mewnet_subs *db = opened.db;

    /* The default "All" group always exists; its id is what other calls take. */
    int64_t group = 0;
    mewnet_error err = mewnet_subs_default_group(db, &group);
    if (err.code != MEWNET_ERROR_NONE) {
        fprintf(stderr, "group: %s\n", err.message);
        mewnet_subs_close(db);
        return 1;
    }

    err = mewnet_subs_add_subscription_in_group(db, group,
                                                "https://example.com/sub.txt",
                                                100 /* priority */, "main");
    if (err.code != MEWNET_ERROR_NONE) {
        fprintf(stderr, "add: %s\n", err.message);
        mewnet_subs_close(db);
        return 1;
    }

    /* add_* does not return the new id, so ask for the next unfetched one. */
    int64_t sub = 0;
    err = mewnet_subs_next_subscription_id(db, group, true /* unfetched_only */, &sub);
    if (err.code != MEWNET_ERROR_NONE || sub == 0) {
        fprintf(stderr, "no subscription to fetch: %s\n", err.message);
        mewnet_subs_close(db);
        return 1;
    }

    /* Downloads, parses plain or base64 bodies, deduplicates and stores. */
    mewnet_subs_fetch_result fetched = mewnet_subs_fetch(db, sub);
    if (fetched.error.code != MEWNET_ERROR_NONE) {
        fprintf(stderr, "fetch: %s\n", fetched.error.message);
    } else {
        printf("HTTP %d: added=%zu updated=%zu removed=%zu skipped=%zu\n",
               fetched.http_status, fetched.added, fetched.updated,
               fetched.removed, fetched.skipped);
    }

    mewnet_subs_stats stats = mewnet_subs_get_stats(db);
    printf("%lld subscriptions, %lld proxies (%lld alive, %lld dead), %lld tests\n",
           (long long)stats.subscriptions, (long long)stats.proxies,
           (long long)stats.alive, (long long)stats.dead, (long long)stats.tests);

    mewnet_subs_close(db);
    return 0;
}
```

`mewnet_subs_fetch` respects the shared request budget; `mewnet_subs_fetch_now`
skips it, and `mewnet_subs_fetch_due` picks whichever subscription is due next —
that is what a scheduler needs. A second thread should get its own connection
with `mewnet_subs_clone(db)` rather than sharing this handle.

### Groups, and picking a node from all subscriptions

A group is a folder for subscriptions: each subscription belongs to exactly one
group and its configs inherit that group. `mewnet_subs_default_group` returns the
id of the **lowest-id group**, created as `All` on a fresh database, and
`mewnet_subs_add_subscription` puts new subscriptions there. It is a default
destination, not a live aggregate — a subscription moved to the `work` group is
no longer in `All`.

To work with everything you have, pass **`group_id = 0`**, which means "no group
filter", to the read and test functions:

```c
/* Counts, listing and selection accept 0 = all groups. */
int64_t alive_all = 0;
mewnet_subs_count_alive(db, 0, &alive_all);

mewnet_subs_proxy best;
mewnet_error picked = mewnet_subs_random_best_alive(db, 0 /* all groups */,
                                                    0 /* exclude */, 0 /* avoid */,
                                                    5 /* candidates */, &best);
if (picked.code == MEWNET_ERROR_NONE) {
    mewnet_add_options add = mewnet_add_options_default();
    add.select_proxy = true;
    mewnet_add_proxy(instance, best.link, &add);   /* main proxy for this process */
}
```

The same `0` works for `count_proxies`, `count_untested`, `count_subscriptions`,
`max_last_fetch`, `load_proxies`, `load_proxy_summaries`, `load_subscriptions`,
`first_alive`, `next_alive`, `test_batch` and `next_subscription_id`. Writers are
the exception: `add_subscription` and `add_subscription_in_group(db, 0, ...)` put
the subscription into the default group, and adding a config always needs a real
group id.

If you keep everything in the default group — the common case — the id is
interchangeable with `0`, and both forms are fine.

### Testing stored configs and routing traffic through the winner

Testing is done by the core (`mewnet_instance`), results and ratings are written
by the storage layer. `mewnet_subs_test_batch` checks many configs at once and
stops as soon as one works.

```c
/* Test stored configs, then route the local mixed inbound through the best. */
#include <stdio.h>
#include "mewnet.h"
#include "mewnet_subs.h"

int main(void) {
    mewnet_config core_config = mewnet_config_default();
    core_config.mixed_port = 0;                 /* pick a free local port */
    mewnet_init_result init = mewnet_init(&core_config);
    if (init.instance == NULL) {
        fprintf(stderr, "core: %s\n", init.error.message);
        return 1;
    }

    mewnet_subs_config config = mewnet_subs_config_default();
    config.db_path = "data/subs.db";
    config.request_limit = 0;                   /* the batch paces itself */
    mewnet_subs_open_result opened = mewnet_subs_open(&config);
    if (opened.db == NULL) {
        fprintf(stderr, "open: %s\n", opened.error.message);
        mewnet_close(init.instance);
        return 1;
    }

    /* Up to 200 configs, 32 checks in parallel. 0 means "every group", so the
       batch covers all subscriptions. Cancelled checks report
       MEWNET_ERROR_CANCELLED and must not be read as failures. */
    mewnet_subs_batch_test_result batch =
        mewnet_subs_test_batch(opened.db, init.instance, 0 /* all groups */, 200, 32);
    printf("tested=%zu alive=%zu first=%s\n",
           batch.tested, batch.alive, batch.first_alive_name);

    /* Highest rating, then lowest delay; the pick is random inside that
       window of 5 candidates so repeated calls do not stick to one node. */
    mewnet_subs_proxy chosen;
    mewnet_error pick = mewnet_subs_random_best_alive(opened.db, 0 /* all groups */,
                                                      0 /* exclude id */,
                                                      0 /* avoid id */,
                                                      5 /* candidates */,
                                                      &chosen);
    if (pick.code != MEWNET_ERROR_NONE) {
        printf("nothing alive yet: %s\n", pick.message);
    } else {
        mewnet_add_options add = mewnet_add_options_default();
        add.select_proxy = true;                /* becomes the main proxy */
        mewnet_add_proxy(init.instance, chosen.link, &add);
        printf("http://127.0.0.1:%d -> %s (%u ms)\n",
               mewnet_get_status(init.instance).mixed_port,
               chosen.name, chosen.last_delay_ms);
    }

    mewnet_subs_close(opened.db);
    mewnet_close(init.instance);
    return 0;
}
```

For a single step instead of a batch, use `mewnet_subs_test_next` (waits for the
rate limiter) or `mewnet_subs_test_next_now` (does not). `mewnet_subs_test_proxy`
tests one known id, and `mewnet_subs_record_test` stores a result you measured
elsewhere. Health is separate: `mewnet_subs_record_health` orders candidates but
never flips `alive`.

### Paging through stored configs

A database with tens of thousands of configs must never be loaded whole. The
summary variant returns rows without the share link, which is what a list view
needs; `mewnet_subs_get_proxy` fetches the full row for the selected item.

```c
/* Page through stored configs: 50 rows per page, no full-table load. */
#include <stdio.h>
#include "mewnet_subs.h"

int main(void) {
    mewnet_subs_config config = mewnet_subs_config_default();
    config.db_path = "data/subs.db";
    mewnet_subs_open_result opened = mewnet_subs_open(&config);
    if (opened.db == NULL) {
        fprintf(stderr, "open: %s\n", opened.error.message);
        return 1;
    }

    int64_t group = 0;
    mewnet_subs_default_group(opened.db, &group);

    size_t offset = 0;
    for (;;) {
        mewnet_subs_proxy_summary_list page =
            mewnet_subs_load_proxy_summaries(opened.db, group, 50, offset);
        if (page.error.code != MEWNET_ERROR_NONE) {
            fprintf(stderr, "page: %s\n", page.error.message);
            mewnet_subs_unload_proxy_summaries(page);
            break;
        }
        if (page.count == 0) {
            mewnet_subs_unload_proxy_summaries(page);   /* done */
            break;
        }
        for (size_t i = 0; i < page.count; i++) {
            mewnet_subs_proxy_summary *p = &page.items[i];
            printf("%-32s %-8s %s:%d rating=%d alive=%d %u ms\n",
                   p->name, p->protocol, p->server, p->port,
                   p->rating, p->alive, p->last_delay_ms);
        }
        offset += page.count;
        mewnet_subs_unload_proxy_summaries(page);       /* lists are library-owned */
    }

    /* Full row (with the link) only for the item the user selected. */
    mewnet_subs_proxy full;
    if (mewnet_subs_get_proxy(opened.db, 1, &full).code == MEWNET_ERROR_NONE) {
        printf("first row link: %.60s...\n", full.link);
    }

    mewnet_subs_close(opened.db);
    return 0;
}
```

`mewnet_subs_count_proxies`, `mewnet_subs_count_alive` and
`mewnet_subs_count_untested` return counters as SQL aggregates, so a status bar
never needs the rows. Manual configs added with `mewnet_subs_add_proxy` survive
subscription refreshes; `mewnet_subs_remove_proxy` deletes by id.

### Running the discovery agent

The agent is the loop the CLI and the GTK panel use: refresh subscriptions on
schedule, test untested configs, keep a pool of working nodes, health-check the
active one and fail over.

```c
/* Run the agent for 30 seconds and print what it does. */
#include <stdio.h>
#include <unistd.h>
#include "mewnet.h"
#include "mewnet_subs.h"

/* Both callbacks run on agent threads: print and return, do not tear anything
   down from inside them. */
static void on_log(const char *message, void *user) {
    (void)user;
    printf("[agent] %s\n", message);
}

static void on_state(const mewnet_agent_snapshot *s, void *user) {
    (void)user;
    if (s->proxy_id == 0) {
        return;                    /* nothing selected yet */
    }
    printf("[state] %s (%s %s:%d) %u ms | alive=%lld untested=%lld ready=%d\n",
           s->name, s->protocol, s->server, s->port, s->delay_ms,
           (long long)s->alive, (long long)s->untested, s->ready ? 1 : 0);
}

int main(void) {
    mewnet_config core_config = mewnet_config_default();
    core_config.mixed_port = 0;
    mewnet_init_result init = mewnet_init(&core_config);
    if (init.instance == NULL) {
        fprintf(stderr, "core: %s\n", init.error.message);
        return 1;
    }

    mewnet_subs_config config = mewnet_subs_config_default();
    config.db_path = "data/subs.db";
    mewnet_subs_open_result opened = mewnet_subs_open(&config);
    if (opened.db == NULL) {
        fprintf(stderr, "open: %s\n", opened.error.message);
        mewnet_close(init.instance);
        return 1;
    }

    int64_t group = 0;
    mewnet_subs_default_group(opened.db, &group);

    mewnet_agent_config ac = mewnet_agent_config_default();
    ac.db = opened.db;
    ac.milib = init.instance;
    ac.group_id = group;
    ac.proxy_port = mewnet_get_status(init.instance).mixed_port;
    ac.batch = 50;               /* fast discovery batch */
    ac.concurrency = 30;         /* parallel checks inside a batch */
    ac.pool_target = 4;          /* working nodes to keep in reserve */
    ac.health_interval_sec = 6;
    ac.health_url = "https://www.gstatic.com/generate_204";
    ac.health_expected = "204";

    mewnet_agent_callbacks callbacks = {on_log, on_state, NULL};
    mewnet_agent *agent = mewnet_agent_start(&ac, callbacks);
    if (agent == NULL) {
        fprintf(stderr, "agent failed to start\n");
        mewnet_subs_close(opened.db);
        mewnet_close(init.instance);
        return 1;
    }

    for (int i = 0; i < 30; i++) {
        sleep(1);
        /* A UI would call mewnet_agent_request_switch(agent) from a button. */
    }

    /* Read the state while the agent is alive: stop frees the handle. */
    mewnet_agent_snapshot snapshot = mewnet_agent_get_snapshot(agent);
    printf("final: %s ready=%d\n", snapshot.name, snapshot.ready ? 1 : 0);

    mewnet_agent_stop(agent);    /* stops the thread, closes its connections, frees the handle */

    mewnet_subs_close(opened.db);
    mewnet_close(init.instance);
    return 0;
}
```

`run_seconds` in the config stops the agent by itself; `0` means "run until
`mewnet_agent_stop`". `mewnet_agent_request_subscription(agent, id)` forces a
fetch of one subscription, which is what "refresh" buttons use.

### Rules that apply to all of it

- **Errors are values.** `mewnet_error` has `code` and `message`; `mewnet_init`
  and `mewnet_subs_open` return it in the result struct, other functions return
  the code and push the text to the log callback. Check `.code == MEWNET_ERROR_NONE`.
- **Ownership.** `mewnet_subs_close(db)` closes a connection; every
  `mewnet_subs_load_*` result goes back through the matching `mewnet_subs_unload_*`;
  `mewnet_agent_stop` frees the agent thread; core handles belong to `mewnet_close`.
- **Threads.** One `mewnet_subs*` per thread — take a second one with
  `mewnet_subs_clone`. The agent owns its own connection. Callbacks must not call
  `mewnet_agent_stop`, `mewnet_subs_close` or `mewnet_close`.
- **Defaults you can rely on:** 10 requests per 240 s (`request_limit`,
  `request_window_seconds`), 6 h refresh (`default_fetch_interval_sec`), test URL
  `generate_204` with a 5 s timeout.

Function families, if you need to look something up quickly: storage
(`config_default`, `open`, `close`, `clone`, `get_stats`), groups (`load_groups`,
`add_group`, `remove_group`, `set_group_priority`, `set_subscription_group`,
`default_group`), subscriptions (`add_subscription*`, `remove_subscription`,
`set_priority`, `set_enabled`, `load_subscriptions*`, `count_subscriptions`,
`max_last_fetch`, `next_subscription_id`), configs (`load_proxies`,
`load_proxy_summaries`, `get_proxy`, `add_proxy`, `remove_proxy`,
`count_proxies`), fetching (`fetch`, `fetch_now`, `fetch_due`, `cancel_fetch`),
testing (`test_batch`, `test_next`, `test_next_now`, `test_candidate_now`,
`test_proxy`, `record_test`, `record_health`), selection (`first_alive`,
`next_alive`, `random_best_alive`), limiter (`rate_limit_wait`), agent
(`agent_config_default`, `agent_start`, `agent_stop`, `agent_request_switch`,
`agent_request_subscription`, `agent_get_snapshot`).

### The SQLite side

The file is plain SQLite with WAL, `busy_timeout=5000`, a 4 MB page cache,
`temp_store=FILE`, foreign keys with `ON DELETE CASCADE`, and FTS5 compiled in.
Tables: `subscription_groups`, `subscriptions`, `proxies`, `proxy_sources`,
`test_events` — columns and indexes are in
[`docs/SUBSCRIPTIONS.md`](docs/SUBSCRIPTIONS.md#sqlite).

Read it with any tool while the app runs (WAL allows concurrent readers):

```bash
sqlite3 data/subs.db '.tables'
sqlite3 data/subs.db 'SELECT protocol, COUNT(*), SUM(alive) FROM proxies GROUP BY protocol;'
sqlite3 data/subs.db 'SELECT name, rating, last_delay_ms FROM proxies ORDER BY rating DESC LIMIT 5;'
```

Do not write to the file from another process while the agent is running. If an
embedder needs its own SQL — an FTS5 index over proxy names, for example — the
library exports the `sqlite3_*` symbols of this same build, so you can open the
file and query it without linking a second SQLite copy.

## C API: maunet, global state with link ids

`maunet` is a raylib-style façade over everything above: one hidden instance,
one database, one discovery agent, and an id for every link. Application code
never sees handles — it passes link ids. Header:
[`maunet.h`](libs/mewnet/cmd/mewnet/maunet.h).

```c
#include <stdio.h>
#include "maunet.h"

int main(void) {
    maunet_settings settings = MaunetSettingsDefault();
    settings.db_path = "data/app.db";   /* created and migrated on init */
    settings.mixed_port = 0;            /* free local port */
    settings.run_agent = true;          /* keep a working pool in the background */

    if (!InitMaunet(&settings)) {
        fprintf(stderr, "init: %s\n", GetMaunetError());
        return 1;
    }

    /* A subscription stores its configs as links; a single link can be added too. */
    maunet_link_result sub = AddMaunetSubscription("https://example.com/sub.txt");
    printf("subscription %lld, %lld links known\n", (long long)sub.id,
           (long long)GetMaunetLinkCount());

    maunet_link_result manual = AddMaunetLink("vless://...");
    if (manual.id > 0 && SetMaunetLinkAsDefault(manual.id)) {
        printf("default link: %s, proxy http://127.0.0.1:%d\n",
               GetMaunetLink(manual.id).name, GetMaunetProxyPort());
    }

    /* Measure one link; alive, delay and rating are stored with it. */
    if (!TestMaunetLink(manual.id)) {
        printf("link is not usable: %s\n", GetMaunetError());
    }

    maunet_status status = GetMaunetStatus();
    printf("%lld links, %lld alive, %lld subscriptions, selected %s\n",
           (long long)status.links, (long long)status.alive,
           (long long)status.subscriptions, status.selected);

    DeinitMaunet();          /* stops the agent and closes the core */
    return 0;
}
```

Every call reports failures the same way: `false` (or `id == 0`) plus a message
from `GetMaunetError()`.

**Settings** (`MaunetSettingsDefault` fills all of them): `db_path`,
`mixed_port`, `log_level`, `user_agent`, `test_url`, `test_expected`,
`test_timeout_ms`, `health_url`, `health_expected`, `health_timeout_ms`,
`request_limit`, `request_window_seconds`, `fetch_interval_sec`, `pool_target`,
`batch`, `concurrency`, `run_agent`. Zero and NULL fields mean "keep the default".

**Functions**

- lifecycle: `MaunetSettingsDefault`, `InitMaunet`, `DeinitMaunet`,
  `IsMaunetReady`, `GetMaunetError`, `GetMaunetStatus`, `GetMaunetProxyPort`;
- links: `AddMaunetLink`, `RemoveMaunetLink`, `SetMaunetLinkAsDefault`,
  `GetMaunetDefaultLink`, `TestMaunetLink`, `GetMaunetLink`,
  `LoadMaunetLinks`/`UnloadMaunetLinks`, `GetMaunetLinkCount`;
- subscriptions: `AddMaunetSubscription`, `RemoveMaunetSubscription`,
  `RefreshMaunetSubscriptions`, `LoadMaunetSubscriptions`/
  `UnloadMaunetSubscriptions`, `GetMaunetSubscriptionCount`.

**Behaviour that matters**

- a link id is the stored row id, so it survives restarts and is the same value
  that `LoadMaunetLinks` returns;
- adding a link that already exists never creates a second row: the same link
  text, or the same server and credentials under another name or with different
  transport options, returns the id of the stored link;
- `AddMaunetLink` writes a manual config: subscriptions never remove it, and
  `AddMaunetSubscription` never overwrites it;
- links are registered in the core lazily — `SetMaunetLinkAsDefault` and
  `TestMaunetLink` do it when needed, so loading ten thousand links costs
  nothing;
- the agent may move the selection; `GetMaunetDefaultLink` follows it and
  returns the id of whatever is selected;
- `run_agent` requires an inbound (`mixed_port >= 0`);
- `InitMaunet`/`DeinitMaunet` must come from one thread; the other calls are
  serialized by a library mutex and may be used from a worker.

Try it: `make test-maunet`.

## Async

Every check has an async variant: pass a callback, or `NULL` and poll.

```c
static void on_ping(const mewnet_task_result *result, void *user) {
    if (result->state == MEWNET_TASK_DONE && result->error == MEWNET_ERROR_NONE) {
        printf("delay: %u\n", result->ping.delay_ms);
    }
}

mewnet_task *task = mewnet_link_ping_async(link, &delay, on_ping, NULL);
/* ... */
mewnet_task_destroy(task);
```

Callbacks run on an arbitrary OS thread; do not call `mewnet_close`,
`mewnet_link_destroy` or `mewnet_task_destroy` from inside one. For a
main-thread loop, create the task with `NULL` and use `mewnet_task_poll`.

## Clients

**`mewnet-cli`** — the agent with a TUI (`s` switches proxy, `q` quits) or
`--headless` for scripts.

**`mewnet-watch`** — discovery only: checks up to 70 proxies at once until one
works, prints `PROXY http://127.0.0.1:<port>`.

**`mewnet-gtk`** — GTK4 panel: add and remove subscriptions, change the active
node, watch the discovery log.

**`mewnet-gui`** — a small Odin/raylib GUI for browsing groups and paging
through proxies (`make gui`).

**`mewnet-discovery`** — a probe for the discovery pipeline itself. It fetches
subscriptions and grows the pool in three stages: wide batches until the first
`--fast` nodes work, smaller paced batches up to `--bulk`, then a slow
background trickle. It reports time to the first node, time per stage, success
rate, protocol mix, delay percentiles, and finally routes traffic through the
best node to prove it works:

```bash
make subs
./build/apps/mewnet-discovery/mewnet-discovery --db data/discovery.db \
  --fast 8 --bulk 100 --run-seconds 300 \
  https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge.txt \
  https://raw.githubusercontent.com/barry-far/V2ray-Config/main/All_Configs_Sub.txt
```

```text
== stage 1 done: 5 alive in 7.9 s (first node after 1.9 s)
== stage 2: grow to 100 proxies (batch 24, concurrency 12, pause 500 ms)
...
verify through      : GB-node -> HTTP 204 in 670 ms
```

Details and every flag: [`apps/mewnet-discovery/README.md`](apps/mewnet-discovery/README.md).

All of them use the same library and the same agent, so the behaviour matches.
Policy (health checks every 6 s, failover after 2 failures out of 3, pool of 12,
random switch among the best): [`docs/GTK_RELIABILITY.md`](docs/GTK_RELIABILITY.md).

## Build

Requirements: Linux x86_64 with glibc, Go 1.26+ with CGO, a C compiler.
Consumers of the finished library need none of that — only `libmewnet.so` and
the headers.

```bash
make deps            # install go/clang/cmake/odin via mise
make build           # dist/libmewnet.so + dist/libmewnet.h
make subs            # the same library + C clients and tests
```

Cross-compile the core for Windows:

```bash
make build-mingw-core    # dist/windows/mewnet.dll + import library
make subs-mingw          # one DLL for all clients
```

Full toolchain notes, flags and caveats: [`docs/BUILD.md`](docs/BUILD.md).

## Choose your entry point

| You want | Start here |
|---|---|
| A proxy on localhost from a subscription | `mewnet-cli` (Quick start) or the GTK panel |
| Proxy control from C | [`mewnet.h`](libs/mewnet/cmd/mewnet/mewnet.h), [`docs/USAGE.md`](docs/USAGE.md) |
| Global state and link ids in C (raylib style) | [`maunet.h`](libs/mewnet/cmd/mewnet/maunet.h), section below |
| Proxy control from Python | [`docs/USAGE.md`](docs/USAGE.md#python) |
| Proxy control from Go | [`libs/mewnet/README.md`](libs/mewnet/README.md) |
| Subscription storage and rating | [`docs/SUBSCRIPTIONS.md`](docs/SUBSCRIPTIONS.md) |
| One proxy with its own SOCKS5 auth | [`docs/SHARED_VPN_LINK.md`](docs/SHARED_VPN_LINK.md) |
| How the agent picks and switches nodes | [`docs/GTK_RELIABILITY.md`](docs/GTK_RELIABILITY.md) |
| Build and cross-compile details | [`docs/BUILD.md`](docs/BUILD.md) |

## Repository layout

```text
libs/mewnet/              Go core, built as the c-shared libmewnet
libs/mewnet/cmd/mewnet/   one Go package: Go core + all C subscription code + SQLite
apps/gui/                 Odin + raylib GUI
apps/mewnet-gtk/          C + GTK4 control panel
apps/mewnet-cli/          C CLI/TUI agent
apps/mewnet-watch/        C discovery tool
apps/mewnet-discovery/    C staged discovery probe (timings, success rate)
tests/                    C, Python and e2e tests
docs/                     usage, building, testing, design notes
```

## Limitations

- one mihomo instance per process (the core keeps global state);
- `dlclose` on the `.so` is not allowed: the Go runtime cannot be unloaded;
- no TUN/system VPN in this build (needs the `with_gvisor` tag);
- "ping" is HTTP latency via `URLTest`, not ICMP;
- per-link SOCKS5 proxies TCP only;
- switching the selected proxy does not migrate connections that are already
  open — they finish on the old node;
- `mewnet_close` and `mewnet_link_destroy` must not be called from an async
  callback;
- a panic in a background mihomo goroutine kills the process — run untrusted
  configs in a separate process;
- the build depends on glibc; for old distributions build in an older image.

## Tests

```bash
make test-unit       # ABI and logic through ctypes
make test-links      # SharedVPNLink without network (auth, errors, destroy)
make test-c          # C test with #include "mewnet.h"
make test-maunet     # maunet global-state facade: link ids, defaults, subscriptions
make test-stress     # 100 add/remove cycles, FD/goroutine/RSS
make test-agent      # offline failover/recovery test for the agent
make xray            # download Xray for e2e (into /tmp, not committed)
make test-e2e        # local Xray + VLESS + mixed inbound
make test-e2e-links  # 4 proxies, hot-swap, concurrent pings
make test-e2e-subs   # local Xray + HTTP subscription + SQLite/rating
make test-c-ping-local
```

Results, numbers and known gaps: [`docs/TEST_REPORT.md`](docs/TEST_REPORT.md).

## License

GPL-3.0, same as mihomo. Distributing the `.so` means complying with the GPL.
