# mewnet — test report

Date: 2026-11-05
Version: 0.2.0 (ABI v2), mihomo v1.19.33

Below (§1–§13) is the original run on Fedora 45. The full re-verification on
another machine (Ubuntu 24.04, gcc) is in §14 dated 2026-11-16, which also
holds the current numbers and environment limitations.

## Environment

| Parameter | Value |
|---|---|
| OS | Fedora Linux 45 (Workstation) |
| Kernel | 7.2.11-300.fc45.x86_64 |
| libc | glibc 2.44 |
| Compiler | clang 23.1.4 (mise), gcc 16.2.1 |
| Go | 1.27.3 linux/amd64 (mise) |
| CMake | 4.4.5 (mise) |
| Python | 3.15.0 |
| Xray (e2e) | 26.10.31 |

Build:

```bash
export PATH="$HOME/.local/go/bin:$PATH"
export GOPATH="$HOME/.cache/mewnet/gopath"
export GOCACHE="$HOME/.cache/mewnet/gocache"
CC=gcc CGO_ENABLED=1 go build -p=1 -trimpath -buildmode=c-shared \
  -ldflags "-s -w -X github.com/metacubex/mihomo/constant.Version=v1.19.33" \
  -o dist/libmewnet.so .
```

Result: `dist/libmewnet.so` — 55 MiB.

## 1. Go task tests

Command: `go test ./internal/core`

Verified:

- `cancelLinks` cancels the task and waits for the callback to finish;
- `cancelLinks` does not touch tasks of other links;
- `FreeTask` does not return while the callback is still running.

```
ok  	mewnet/internal/core
```

## 2. Basic ABI/unit tests (Python ctypes)

Command: `python3 tests/python/test_api.py`

```
Ran 10 tests
OK
```

Covered: version/ABI, init/status, invalid links, add/load/remove,
built-in name protection, name uniquification, select, delay error for a dead
proxy, repeated init after shutdown, refusal of a second instance.

## 3. SharedVPNLink without network

Command: `python3 tests/python/test_links.py`

```
Ran 14 tests
OK
```

Covered:

- creating a `SharedVPNLink` and `mewnet_link_get_info`;
- generation of the per-link SOCKS5 login/password;
- manual password (`password`) and upstream password (`proxy_password`);
- SOCKS5 auth: a wrong password is rejected, the correct one is accepted;
- `local_proxy = false` disables the local SOCKS;
- ping/speed/exit_ip for a dead proxy return an error without a crash;
- `mewnet_link_ping_many` for three dead links: three isolated results;
- async ping/speed/exit_ip with a C callback;
- polling an async task and `mewnet_task_destroy`;
- `mewnet_remove_proxy` refuses to delete a proxy owned by a link;
- `mewnet_link_destroy`: the link disappears from the registry, the SOCKS port closes.

## 4. C ABI

Command: `make -C tests test-c`

```
PASS  abi version
PASS  version
PASS  init
PASS  status ok
PASS  add proxy
PASS  load proxies
PASS  added proxy is selected
PASS  select DIRECT
PASS  remove proxy
PASS  remove missing fails
PASS  link create
PASS  link info
PASS  link info has auth
PASS  load links
PASS  load links after destroy

all C tests passed
```

The test is built with `#include "mewnet.h"` and links against `libmewnet.so`.

Ping check from C through a local Xray:

```
$ make -C tests test-c-ping-local
2
```

`2` is the delay in milliseconds from `mewnet_ping_result.delay_ms`.

## 5. Stress: 100 add/remove cycles

Command: `python3 tests/python/test_stress.py`

```
cycles=100 fds=11->11 goroutines=5->5 rss=60760KiB->65172KiB
OK
```

No FD or goroutine leaks; RSS grows by ~4.3 MiB per 100 hot-swap cycles.

## 6. E2E: single VLESS + mixed inbound

Command: `python3 tests/e2e/run.py` → **18 passed, 0 failed**.

Confirmed:

- delay through VLESS > 0 (local `204` and gstatic);
- `curl -x http://127.0.0.1:<mixed>` really goes through Xray (the access log grows);
- after `remove` traffic returns to `DIRECT` and Xray is no longer used;
- with Xray stopped, delay and traffic return an error without a crash;
- async callback, `test_link` without adding, log callback.

## 7. E2E: 4 proxies, hot-swap and concurrent tasks

Command: `python3 tests/e2e/run_links.py` → **39 passed, 0 failed**.

Scenario:

1. Python creates 4 `SharedVPNLink` instances over FFI; each link points to
   a separate local Xray with its own access log.
2. For each link `ping`, `speed` (1 MiB) and `exit_ip` were verified.
3. Per-link SOCKS5: a wrong password is rejected, the correct one routes
   the request through its own Xray.
4. Hot-swap: proxy #1 is selected, a slow download is started, then the active
   proxy is switched to #2. The download completed in full and its traffic stayed
   in the access log of Xray #1; the new request went to Xray #2.
5. Concurrency: while the slow download runs through proxy #1,
   `mewnet_link_ping_many` pings three others. All three are `ok`, and at that
   moment the download is still running and afterwards completes in full.
6. Async `mewnet_link_ping_many_async` with a C callback returns `ok` for all three;
   the same scenario through `mewnet_task_poll` also returns `ok`.
7. `mewnet_link_destroy` for one link: it disappears from the registry, its SOCKS port
   is closed, the running async task is cancelled, and the remaining links keep working.

Output excerpt:

```
PASS  xray-1: ping through proxy
PASS  xray-1: speed through proxy
PASS  xray-1: exit ip through proxy
...
PASS  async many-ping results are ok
PASS  many-ping poll results are ok
PASS  link task is running
PASS  destroy link
PASS  destroy cancels the link task
PASS  pinging a destroyed link fails
PASS  surviving links still work

39 passed, 0 failed
```

## Limitations

- TUN/system VPN was not tested (build without the `with_gvisor` tag);
- only Linux x86_64/glibc 2.44 was verified;
- per-link SOCKS5 — TCP only;
- an external real VLESS link is verified optionally via
  `MEWNET_TEST_VLESS`;
- "ping" is an HTTP delay, not ICMP;
- one core instance per process;
- `mewnet_close` and `mewnet_link_destroy` must not be called from an async callback;
- Xray 26.x blocks loopback/private targets from the VLESS inbound by default;
  the test config adds `finalRules: allow 127.0.0.0/8`.

## 8. Subscriptions (`libmewnet`)

Build:

```bash
make -C apps subs
make -C tests test-subs
```

CTest:

```
1/3 Test #1: base64 ........................... Passed
2/3 Test #2: sha256 ........................... Passed
3/3 Test #3: rating ........................... Passed

100% tests passed out of 3
```

Verified manually:

- a plain subscription with `#profile-title`, `#profile-update-interval`,
  `#subscription-userinfo`;
- a base64 subscription, including ones with line breaks and without padding;
- real GitHub subscriptions:
  - `barry-far/.../All_Configs_base64_Sub.txt` — 9167 configs,
    ~2.7 s, RSS ~48 MB;
  - `ALIILAPRO/.../server.txt` — 4323 configs;
  - `Pawdroid/Free-servers/main/sub` — 9 configs;
- a repeated fetch with `ETag` returns `304` and does not change the DB;
- changing the subscription content deletes stale configs (`removed=2`);
- a test of a working proxy through a local Xray: `alive=1`, `rating 0->1`,
  the name/server/port metadata was updated;
- a test of a dead proxy: `rating 0->-1`, a repeated test on the same day `+0`;
- all results are written to `test_events`;
- `libmewnet` exports only `mewnet_subs_*` symbols.

E2E:

```bash
make -C tests test-e2e-subs
```

```
20 passed, 0 failed
```

Verified: plain/base64 ingest, the `All` group, deduplication of configs between
subscriptions of the same group, `#name` does not affect identity, a password change creates a new record,
a manual proxy (`manual=1`) survives a repeated fetch with generation-cleanup,
a working proxy
through a local Xray (`alive=1`, `rating 0->1`, metadata), a dead proxy
(`rating 0->-1`, a repeated test on the same day does not change the rating),
`test_events`.

## 9. GUI on Odin/raylib

Build:

```bash
make -C apps gui
```

Verified:

- `odin build` with `vendor:raylib` and `libmewnet`;
- a 1200x760 window starts on the current DISPLAY;
- the GUI uses `mewnet_subs_load_proxy_summaries` (a page without `link`) and
  `mewnet_subs_get_proxy` only for the selected record;
- adding/removing subscriptions and manual proxies goes through the public
  `libmewnet` API, without direct SQL queries from the GUI;
- manual proxies (`manual=1`) are not deleted by generation-cleanup during a fetch.

## 10. mingw cross-compilation

```bash
make -C apps build-mingw-core
make -C apps subs-mingw
```

Produced:

- `dist/windows/mewnet.dll` — PE32+ x86-64;
- `dist/windows/libmewnet.dll.a` — import library;
- `dist/windows/mewnet.dll` — PE32+ x86-64;
- `build-mingw/apps/mewnet-subs/mewnet-subs.exe` — PE32+ x86-64.

Running under Wine was not verified.

## 11. Discovery CLI `mewnet-watch`

Check without Xray, on a public GitHub subscription.

Command:

```bash
./build/apps/mewnet-watch/mewnet-watch \
  --db /tmp/opencode/final-watch.db --proxy-port 18085 \
  --batch 70 --concurrency 70 --timeout 3000 \
  https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub \
  https://raw.githubusercontent.com/ALIILAPRO/v2rayNG-Config/main/server.txt \
  https://raw.githubusercontent.com/barry-far/V2ray-Config/main/All_Configs_base64_Sub.txt
```

Result:

```text
ready=1 ready_ms=3412
curl_rc=0 total_ms=4085
HTTP/1.1 200 Connection established
HTTP/1.1 200 OK
<TITLE>The Programming Language Lua
```

Log excerpt:

```text
DB /tmp/opencode/final-watch.db
PROXY http://127.0.0.1:18085
FETCH t=... id=1 status=200 added=7 updated=0 removed=0
BATCH t=... tested=3 alive=1
WORKING id=5 name=新西兰+V2CROSS.COM
PROXY http://127.0.0.1:18085
READY
TEST t=... id=1 alive=1 delay=504 rating=1->1
TEST t=... id=5 alive=1 delay=641 rating=1->1
```

Results:

- a working proxy was found in **3.412 s**;
- `curl https://lua.org` through `http://127.0.0.1:18085` finished
  **4.085 s** after start;
- HTTP `200` and the HTML `The Programming Language Lua` were received;
- Xray was not used;
- after `READY` the next rate-limited test ran about 20 s later,
  i.e. the standard limiter works.

`mewnet-watch` also cross-compiles for mingw:

```text
build-mingw/apps/mewnet-watch/mewnet-watch.exe
```

## 12. GTK4 control panel `mewnet-gtk`

Build:

```bash
make -C apps subs   # builds mewnet-gtk if gtk4.pc is found
```

Run on a fresh database, one public subscription, without Xray:

```bash
./build/apps/mewnet-gtk/mewnet-gtk \
  --db /tmp/opencode/gtk-fresh.db \
  --proxy-port 18097 \
  --batch 70 --concurrency 70 --timeout 3000 \
  https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub
```

Result:

```text
ready_ms=3940
curl_rc=0 total_ms=5237
<TITLE>The Programming Language Lua
```

Window log:

```text
DB /tmp/opencode/gtk-fresh.db
PROXY http://127.0.0.1:18097
batch=70 concurrency=70 fetch_interval=3600s slow_interval=24s
DISCOVERY start batch=70 concurrency=70 timeout=3000ms
FETCH id=1 status=200 added=7 updated=0 removed=0
SUBS n=1
BATCH tested=3 alive=1
ACTIVE id=2 name=新西兰+V2CROSS.COM delay=1366
READY local=http://127.0.0.1:18097
```

Verified:

- with an empty database the window prints `WAIT no subscriptions yet - add a URL`
  ("no subscriptions — add a URL in the window") and waits; after adding/starting with a URL it
  downloads the subscription at once and activates the discovered node;
- with an existing database that has an alive node, the proxy is activated immediately at startup;
- the "Switch proxy" button switches to the next alive node:
  ```text
  SWITCH requested
  ACTIVE id=5 name=新西兰+V2CROSS.COM delay=2814
  SWITCH id=5 name=新西兰+V2CROSS.COM
  ```
- after `READY` discovery leaves the alive nodes alone and tops up the pool: with fewer
  than 4 alive in the pool the step is about 2 s, and 24 s with a full pool;
  ```text
  TEST id=5 alive=1 delay=770 rating=1->1
  TEST id=5 alive=1 delay=2814 rating=1->1
  ```
- `--proxy-port 0` picks a free port and shows it in the window:
  `PROXY http://127.0.0.1:36617`;
- on window close/SIGTERM the process shuts down discovery and Mihomo, and the port is no longer
  listened on.

### 12.1. Automated failover check

```bash
make -C tests test-agent
```

The test is offline: a local `204` origin, three local HTTP proxies, a subscription
to them, and a `mewnet-cli --headless` run. Result:

```text
PASS  bootstrap: READY
PASS  bootstrap: local proxy port
PASS  bootstrap: traffic through proxy
PASS  pool: grew to three working proxies
PASS  switch: moved to another proxy
PASS  switch: random among best, not just two
PASS  switch: traffic still works
PASS  failover: active proxy identified
PASS  failover: health check detected dead proxy
PASS  failover: switched to another proxy
PASS  failover: traffic works after switch
PASS  exhausted: aggressive discovery started
PASS  exhausted: process still alive
PASS  recovery: active proxy selected again
PASS  recovery: traffic works again
PASS  wait: empty database waits
PASS  wait: subscription inserted
PASS  wait: fetched and activated
PASS  wait: traffic works

19 passed, 0 failed
```

Verified: bootstrap, pool growth, random switching among the best, health-check and
automatic failover, complete outage of all proxies, return of a new node,
as well as the "empty database → subscription appeared → automatic start" scenario.
The specification and the failure matrix are in
[`docs/GTK_RELIABILITY.md`](GTK_RELIABILITY.md).

UI memory was verified on databases with 7 and 4226 proxies: RSS ~186 MB and ~190 MB
respectively, i.e. a difference of about 4 MB.

UI smoke without manual clicks:

```bash
./build/apps/mewnet-gtk/mewnet-gtk --db /tmp/ui.db --screenshot /tmp/ui.png
```

The window is rendered to a PNG and the application exits.

## 13. Agent library, dedup and memory scaling

### 13.1. Dedup

On a copy of a working database (21 184 proxies, 30 subscriptions), opening with the new schema:

```text
before:   21184 proxies
after:    12638 proxies
removed:   8546 duplicates
duplicate identities: 0
migration time: ~2.2 s
```

Identity = SHA-256(protocol + host + port + credentials). The name, `#fragment` and
transport options are not taken into account. Unique index:
`proxies(group_id, identity_hash)`.

### 13.2. Memory

```text
30 subscriptions, 12 638 proxies:      GTK ~159 MB RSS
10 030 subscriptions, 12 638 proxies:  GTK ~159 MB RSS
```

The subscription list in the UI is a page of 50 rows; the agent uses SQL counters,
`ORDER BY ... LIMIT 1` and does not load millions of rows. The SQLite cache is limited to
8 MB, `temp_store=FILE`.

### 13.3. Long subscriptions

A subscription is downloaded to a temporary file; the SQLite transaction starts only
after the download. Stopping the CLI while a large subscription is downloading:

```text
exit_ms=108
```

That is, the fetch is cancelled and does not block shutdown/health-check.

### 13.4. Shared agent

`mewnet_agent` in `libmewnet` is used by GTK4 and CLI/TUI. The offline
test `make -C tests test-agent` (previously `test-gtk-agent`) passes 19/19 on the CLI:

```text
bootstrap, pool growth, random switch across 3 nodes, health failover,
total outage, recovery, wait-for-subscription
```

CLI/TUI:

```bash
make -C apps cli
# s — random best proxy, q — quit
```

### 13.5. Health-check through the local proxy

The active node is verified with a real `GET https://lua.org` through
`http://127.0.0.1:<port>` (not just the adapter's `URLTest`). Policy at the time
of this run: 3 failures out of the last 5; the current build uses 2 out of the
last 3 instead. Check on a live application:

```text
ACTIVE id=9792 ...
HEALTH FAIL recent=1/5
HEALTH FAIL recent=2/5
HEALTH FAIL recent=3/5
HEALTH FAIL id=9792 ...
ACTIVE id=4720 ...
```

The failover happened ~15 seconds after the first failure, and the next `curl`
request through the local proxy returned HTTP 200.

Slow discovery runs in a separate thread and a separate SQLite connection,
so even a "stuck" node test does not delay health-check and failover.
The test timeout is additionally bounded in Go (`safeURLTest`) in case the
adapter ignores cancellation.

## 14. Re-verification on Ubuntu 24.04 (2026-11-16)

A full re-verification from scratch on another machine: a clean build, the entire offline
test suite, e2e with a local Xray, and a live CLI run on a public subscription.

### 14.1. Environment

| Parameter | Value |
|---|---|
| OS | Ubuntu 24.04 (LTS) |
| Kernel | 7.3.4-3-liquorix-amd64 |
| libc | glibc 2.39 |
| Compiler | gcc 13.3.0; clang 23.1.4 (mise) for `make -C tests test-c` |
| Go | 1.27.3 linux/amd64 (mise) |
| CMake | 4.4.5 (mise) |
| Python | 3.14.9 |
| Xray (e2e) | 26.11.9 |
| Memory | 5.9 GB + 4.8 GB swap |

Build:

```bash
mise install go@1.27.3 clang@23.1.4 cmake@4.4.5   # android-sdk/odin not needed
CGO_ENABLED=1 CC=gcc go build -p=2 -trimpath -buildmode=c-shared \
  -ldflags "-s -w -X github.com/metacubex/mihomo/constant.Version=v1.19.33" \
  -o dist/libmewnet.so .
```

Result: `dist/libmewnet.so` — 56 MiB, 115 exported `mewnet_*`.

The applications were built with the system compiler:

```bash
cmake -S apps -B build/apps -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc -DMEWNET_BUILD_GTK=OFF
cmake --build build/apps -j1
```

### 14.2. Results

| Suite | Command | Result |
|---|---|---|
| Go tests | `go test ./...` | `ok mewnet`, `ok mewnet/internal/core` |
| Race | `go test -race -count=1 ./...` | `ok mewnet` 1.27s, `ok .../internal/core` 1.16s, no races |
| ABI (ctypes) | `tests/python/test_api.py` | 10/10 |
| SharedVPNLink | `tests/python/test_links.py` | 14/14 |
| C ABI | `tests/c/test_api.c` | 15 checks, all PASS |
| Stress | `tests/python/test_stress.py` | 100 cycles, fd 10→10, goroutines 5→5 |
| Subscriptions | `make -C tests test-subs` | 3/3 (base64, sha256, rating) |
| Agent/CLI offline | `tests/e2e/run_gtk_agent.py` | 19/19 |
| E2E VLESS | `tests/e2e/run.py` | 18/18 |
| E2E 4 proxies | `tests/e2e/run_links.py` | 39/39 |
| E2E subscriptions | `tests/e2e/run_subs.py` | 20/20 |
| C ping, local Xray | `tests/e2e/c_ping_demo.py` | 4 ms |

Stress in numbers:

```text
cycles=100 fds=10->10 goroutines=5->5 rss=77136KiB->81540KiB
```

RSS growth of ~4.3 MiB per 100 hot-swap cycles — the same as in §5.

### 14.3. Live CLI run

`mewnet-cli --headless` on the public subscription
`Pawdroid/Free-servers/main/sub`, port 18199, `pool-target 6`:

```text
== direct IP: 2.26.20.60
== first signal: READY local=
== proxied IP: 163.123.193.24
== http://example.com via proxy: rc=0 code=200
== https://www.gstatic.com/generate_204 via proxy: rc=0 code=204
== after switch, proxied IP: 179.254.191.252
== last STATE: STATE id=6 name=巴西+V2CROSS.COM alive=6 proxies=12 untested=4 ready=1
== active history: ['德国+阿里云', '巴西+V2CROSS.COM']
== exit code: 0
```

What was confirmed:

- `READY` about 20 s after start on 12 configs from the subscription;
- the exit IP changes (`2.26.20.60` → `163.123.193.24`), i.e. traffic really
  leaves through the node, not through DIRECT;
- `HTTP 200` and `204` through the local mixed proxy;
- `SIGUSR1` switches the node, the exit IP changes to `179.254.191.252`,
  and traffic keeps working;
- `SIGTERM` terminates the process with code 0, `Mihomo shutting down` with no errors;
- discovery tops up the pool (`alive` 3→6) and stops at `pool-target`.

Subscription composition and what actually passed traffic: 12 configs —
`ss` (6), `vless` (3), `trojan` (2), `hysteria2` (1); of these, 4 use `security=tls`,
2 use `security=reality`. The alive ones turned out to be `ss` (4), `vless`+TLS (1) and
`hysteria2`+TLS (1) — the active one after the switch was `hysteria2`/TLS, i.e. the
TLS branch was verified with traffic. Both REALITY nodes in this subscription were dead,
so REALITY alone was not confirmed.

### 14.4. Differences from the original run

- the GTK4 panel was not verified: the machine has no `gtk4.pc` (`-DMEWNET_BUILD_GTK=OFF`),
  and the Odin GUI and mingw cross-compilation were not run either;
- the applications were built with gcc, not clang; `-DCMAKE_C_COMPILER=gcc`;
- stale artifacts of the old name (`libmilib.so`,
  `libmilib.h`) were removed from `dist/`.
