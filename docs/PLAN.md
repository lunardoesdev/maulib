# maulib — FFI wrapper for mihomo on Linux

Status: implemented; see `README.md`, `docs/BUILD.md` and `docs/TEST_REPORT.md`.

## 1. Fixed decisions

| Question | Decision |
|---|---|
| Core source | mihomo **v1.19.33**, commit `3f27ef39f8c96bbfbc8b0b9bb10f5c9ef60149d8` |
| Consumer | C ABI + Python `ctypes` for tests |
| Traffic | local mixed SOCKS/HTTP inbound on `127.0.0.1` |
| Test VLESS | local Xray on `127.0.0.1` |
| Add/remove | hot-swap via `tunnel.UpdateProxies` |
| Project license | GPL-3.0 (same as mihomo) |

### Why exactly v1.19.33

At planning time, the `main` branch of the `MetaCubeX/mihomo` repository contains a
Python project rather than the Go core. The tag `v1.19.33` contains the correct Go
source and is available through the Go module proxy. Therefore:

- the version is pinned in `go.mod` and `go.sum`;
- `@latest` and `@main` are not used;
- for a release build, `go mod vendor` or `third_party/mihomo` is allowed.

`go.mod` of mihomo has `replace google.golang.org/protobuf => ...`.
A `replace` from a dependency is not applied, so it must be duplicated in the
project's root `go.mod`.

## 2. Scope of the first version

In scope:

- Linux x86_64, glibc;
- `c-shared` `.so` + C header;
- init/shutdown;
- add/remove/list/select of proxies;
- delay test through a proxy (HTTP HEAD, like the "ping" in the Clash UI);
- async delay tasks;
- local mixed SOCKS/HTTP inbound;
- Xray-based e2e tests;
- C and Python tests, report.

Out of scope:

- TUN and system VPN;
- multiple independent core instances in one process;
- ICMP ping;
- Android/iOS;
- storing/importing subscriptions;
- GUI.

## 3. Architecture

```text
maulib/                        the repository is the library
├── go.mod  go.sum             module of the library
├── main.go  api.go  *.go      package main: cgo, //export
├── *.c  *.h                   C API, subscriptions, SQLite, headers
├── internal/core/             lifecycle, proxy management, tasks
├── golib/                     importable Go API (no cgo)
├── apps/                      clients, GUI, CMake, Makefile
├── tests/                     C, Python, e2e
├── docs/
├── assets/
├── dist/                      build output
└── LICENSE                    GPL-3.0
```

### One instance per process

mihomo keeps its state in the global `tunnel`, `resolver`, `listener` and
`cachefile`. Multiple independent cores in one process are not supported
correctly.

Rules:

- only one handle is active at a time;
- `mewnet_init` returns an error when a handle is already active;
- `mewnet_shutdown` stops listeners and closes proxies and tasks;
- a repeated `mewnet_init` after `shutdown` is tested separately; if it turns out
  to be unstable, it is documented as one-shot per process;
- `dlclose` on the `.so` is prohibited: the Go runtime cannot be unloaded.

### Initialization

1. `constant.SetHomeDir(home)` before any access to mihomo.
2. Load the base YAML.
3. Add/verify the `MEWNET` group (`type: select`, `proxies: [DIRECT]`).
4. Add `MATCH,MEWNET` if there is no `MATCH` in the rules.
5. Choose `mixed-port`:
   - `options.mixed_port > 0` — use it;
   - `options.mixed_port == -1` — inbound disabled;
   - `options.mixed_port == 0` or unset — pick a free port;
   - the value from YAML is used if options does not specify its own.
6. `config.Parse()` → `hub.ApplyConfig(cfg)`.

Default base configuration:

```yaml
allow-lan: false
bind-address: 127.0.0.1
mode: rule
log-level: warning
ipv6: false
proxies: []
proxy-groups:
  - name: MEWNET
    type: select
    proxies: [DIRECT]
rules:
  - MATCH,MEWNET
```

### Hot-swap add/remove

mihomo rules store the adapter **name**, not a pointer, so replacing the group in
`tunnel.Proxies()` does not break `MATCH,MEWNET`.

Add algorithm:

1. `convert.ConvertsV2Ray(link)` → `[]map[string]any`
   (a single link, a list of strings and a base64 subscription are supported).
2. For each element:
   - validate `type`, `server`, `port`;
   - determine the name: fragment → `type-server-port` → suffix on collision;
   - protect the names `DIRECT`, `REJECT`, `GLOBAL`, `MEWNET`;
   - `adapter.ParseProxy(mapping)` for validation.
3. Build a new map: a copy of `tunnel.Proxies()` plus the new proxies.
4. Rebuild `MEWNET`:
   - `provider.NewHealthCheck(proxies, C.DefaultTestURL, 5000, 0, true, nil)`;
   - `provider.NewCompatibleProvider("MEWNET", proxies, hc)`;
   - `outboundgroup.NewSelector(...)`;
   - wrap with `adapter.NewProxy(selector)`.
5. Rebuild `GLOBAL` if it was created automatically.
6. `tunnel.UpdateProxies(newProxies, newProviders)`.
7. Close the old group/provider; preserve the selected item.
8. Update the internal registry (`name`, `type`, `link`, `source`, `added_at`).

Removal:

1. find the proxy in the registry (built-in ones cannot be removed);
2. remember the group's current selection;
3. build a map without the proxy and rebuild the group;
4. `tunnel.UpdateProxies`;
5. close the connections whose chain contains the name being removed
   (`statistic.DefaultManager`);
6. close the proxy and the old group/provider;
7. if the selected proxy was removed, select `DIRECT` (or the first remaining one).

Fallback: a full `executor.ApplyConfig(cfg, false)` if the configuration structure
changes. It is slower and suspends the tunnel, so it is not used in add/remove.

### Delay test

`C.Proxy.URLTest(ctx, url, expectedStatus)`:

- returns the delay in ms;
- `0`/error — failure;
- the timeout is set via `context.WithTimeout`;
- by default the URL is `https://www.gstatic.com/generate_204`, expected `204`;
- for the local test — `http://127.0.0.1:<port>/204`;
- this is an HTTP delay, not ICMP.

`mewnet_test_link` parses the link, creates a temporary proxy, tests it and closes
it without adding it to the state.

### Threads and callbacks

- all public functions are thread-safe except `init/shutdown` (call from a single
  thread);
- async tasks run in Go goroutines;
- the C callback is invoked through a trampoline in `bridge.c`;
- the callback must not call `mewnet_shutdown` and must not throw C++
  exceptions;
- strings in the callback live only for the duration of the call;
- `mewnet_shutdown` cancels active tasks and waits for them to finish.

### Errors and memory

- functions return `MEWNET_OK`/`MEWNET_ERR_FAILED`; the error text is written to
  `mewnet_error.message` (fixed buffer);
- output data are C structures; strings are copied into arrays inside the
  structures, the library does not allocate memory for results;
- lists are written into the caller's buffer, `count` always contains the full
  size;
- all `//export` functions are wrapped in `recover()`;
- a panic in a mihomo background goroutine is not intercepted — this is a
  limitation of an in-process library.

## 4. C API (v2)

ABI v2 is raylib-style: flat functions, value structures, `load/unload` for
lists, opaque handles, no JSON and no thread-local last-error.

Key decisions:

- `mewnet_instance*`, `mewnet_link*`, `mewnet_task*` are opaque handles;
- operations return a value/result that carries the error;
- `mewnet_error` with a code and message is used for `init`/`link_create`;
- `mewnet_error_code` — for frequent operations;
- lists: `mewnet_load_proxies`/`mewnet_unload_proxies`,
  `mewnet_load_links`/`mewnet_unload_links`,
  `mewnet_link_ping_many`/`mewnet_unload_ping_many`;
- async: callback or `mewnet_task_poll`, cancellation via `mewnet_task_cancel`,
  release via `mewnet_task_destroy`;
- `mewnet_link_destroy` cancels and waits for the link's tasks;
- `mewnet_close` cancels and waits for all instance tasks;
- log callback via `mewnet_set_log_callback`.

Main function groups:

- lifecycle: `mewnet_init`, `mewnet_close`, `mewnet_get_status`;
- proxy management: `mewnet_add_proxy`, `mewnet_remove_proxy`,
  `mewnet_select_proxy`, `mewnet_load_proxies`;
- latency: `mewnet_test_delay`, `mewnet_test_link`, `mewnet_link_ping`;
- `SharedVPNLink`: `mewnet_link_create`, `mewnet_link_destroy`,
  `mewnet_link_get_info`, `mewnet_link_ping`, `mewnet_link_speed`,
  `mewnet_link_exit_ip`;
- async: `mewnet_*_async`, `mewnet_task_poll`, `mewnet_task_cancel`,
  `mewnet_task_destroy`.

## 5. Build

Go is not installed on the system. Installation without root:

```bash
# Go 1.26.0 — the version from mihomo CI
curl -LO https://go.dev/dl/go1.26.0.linux-amd64.tar.gz
tar -C "$HOME/.local" -xzf go1.26.0.linux-amd64.tar.gz
export PATH="$HOME/.local/go/bin:$PATH"

export GOPATH="$HOME/.cache/mewnet/gopath"
export GOCACHE="$HOME/.cache/mewnet/gocache"
export GOMODCACHE="$GOPATH/pkg/mod"
```

Build:

```bash
CC=gcc CGO_ENABLED=1 go build \
  -trimpath \
  -buildmode=c-shared \
  -ldflags "-s -w -X github.com/metacubex/mihomo/constant.Version=v1.19.33" \
  -o dist/libmewnet.so .
```

Notes:

- by default without the `with_gvisor` tag (TUN/Tailscale are not needed);
- if TUN is needed — a separate build with `-tags with_gvisor`;
- little memory (~1.2 GB free), build with `-p=1`;
- glibc from Fedora 45 may not be suitable for older distributions; for
  distribution, build in an older base image;
- artifacts: `.so`, `mewnet.h`, sha256, `mewnet.pc`,
  C/Python examples; the mingw build is `dist/windows/mewnet.dll`.

## 6. Testing

### 6.1 Unit / ABI

- valid VLESS link: add → list → remove;
- empty/malformed/non-VLESS link: error, state unchanged;
- duplicate names and fragments: unique names;
- removal of a nonexistent/built-in proxy: error;
- removal of the selected proxy: fallback to `DIRECT`;
- 100 add/remove cycles: FD, goroutine, RSS;
- C test with `#include "mewnet.h"`;
- Python `ctypes`: `NULL`, invalid handles, string release, callback;
- optionally `-race`.

### 6.2 E2E with local Xray

Tools:

- Xray prebuilt, the version is pinned, the binary is not committed;
- a local HTTP server returning `204` for `GET`/`HEAD`;
- mihomo with a mixed-port on a free port.

Scenarios:

| # | Scenario | Expectation |
|---|---|---|
| 1 | add VLESS TCP without TLS → select → delay (local 204) | delay > 0 |
| 2 | delay (`https://www.gstatic.com/generate_204`) | delay > 0 when the network is available |
| 3 | `curl -x socks5h://... http://127.0.0.1:.../204` | 204, the Xray access log contains the connection |
| 4 | stop Xray → delay/curl | clear error, no crash |
| 5 | remove proxy → direct request | works through DIRECT |
| 6 | "dead" proxy (blackhole/unreachable port) | delay timeout, the good proxy still works |
| 7 | VLESS + TLS self-signed (`allowInsecure`) | delay and traffic work |
| 8 | real link from `MEWNET_TEST_VLESS` | delay and traffic work |

The real link is passed only via env/file and never appears in logs or git.

### 6.3 Acceptance criteria

- the `.so` loads from C and Python;
- add/remove/list/select are consistent;
- delay returns ms through the proxy and an error when unreachable;
- an HTTP request through the mixed-port actually traverses VLESS (confirmed by
  the Xray access log);
- removing the proxy returns traffic to DIRECT;
- no crashes on invalid input;
- no unbounded growth of FD/goroutine/RSS over 100 cycles;
- `docs/TEST_REPORT.md` contains commands, numbers and limitations.

## 7. Stages

- [x] **0. Toolchain** — Go 1.26, cgo hello-world `.so`, memory check.
- [x] **1. Skeleton** — module, mihomo pin, init/shutdown, loading the `.so` from Python.
- [x] **2. Config and inbound** — base YAML, mixed-port, `status`, `list`.
- [x] **3. Hot-swap** — add/remove/select, names, errors, resource cleanup.
- [x] **4. Delay** — sync/async, `test_link`, task cancellation.
- [x] **5. E2E** — Xray + local 204 + negative scenarios.
- [x] **6. TLS/REALITY and stress** — stress (100 cycles) is done; TLS is verified
  by traffic (`hysteria2` and `vless` + TLS on a live subscription), the real
  subscription and `go test -race ./...` passed on 2026-11-16
  (`docs/TEST_REPORT.md` §14).
  The REALITY nodes in the test subscription turned out to be dead, so REALITY is
  not separately confirmed.
- [x] **7. Documentation** — Makefile, README, `TEST_REPORT.md`, LICENSE.
- [x] **8. SharedVPNLink** — link object, per-link SOCKS5 with authentication,
  ping/speed/exit_ip, concurrent tasks, hot-swap; e2e on 4 proxies.
- [x] **9. Subscriptions** — the C part inside a single `libmewnet`: streaming
  plain/base64 ingest, SQLite, priority, +1/-1 rating with daily limits, a shared
  rate limit, the `mewnet-subs` CLI.

Estimate: MVP — ~1 working day, full version — 1.5–2 days.

## 8. Risks

1. **Supply chain**: the main mihomo contains the wrong project. Pin `v1.19.33`,
   `go.sum`, vendor if necessary.
2. **GPL-3.0**: distributing a `.so` with mihomo requires GPL compliance.
   The project is chosen as open-source GPL-3.0.
3. **One instance per process**: a consequence of mihomo's global state.
4. **A panic in a background goroutine** crashes the process; untrusted configs
   need a separate process.
5. **Signals**: the Go runtime installs handlers; conflicts with JVM/Node are
   possible.
6. **VLESS converter**: `allowInsecure` is not mapped for VLESS — we patch it in
   the wrapper.
7. **glibc/architecture**: built on Fedora 45; for older systems — a separate
   base image.
8. **Memory during the build**: mihomo is large; `-p=2`, on OOM — swap or another
   machine.
9. **Public subscriptions**: configs are untrusted; run the tester as a separate
   process, do not embed it in a long-lived service without a sandbox.
