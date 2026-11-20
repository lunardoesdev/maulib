# Tests for the library.
#
# The library is built by ../dist/libmewnet.so; the first target below does it
# from the repository root when the file is missing or older than the sources:
#
#   cd .. && go build -buildmode=c-shared -o dist/libmewnet.so .
#
# The C tests are compiled against the headers in the repository root and link
# dist/libmewnet.so with an $ORIGIN rpath, so binaries in dist/ run in place.

ROOT    := $(abspath $(CURDIR)/..)
MISE    ?= mise
GO      ?= go
# The C tests are compiled with clang from mise; `make CC=gcc` also works.
# (:= because make predefines CC=cc, so ?= would never apply.)
CC      := $(MISE) exec -- clang
# cgo's compiler.
CGO_CC  ?= clang
PYTHON  ?= python3

LIB     := $(ROOT)/dist/libmewnet.so
CORE_LDFLAGS := -s -w -X github.com/metacubex/mihomo/constant.Version=v1.19.33

SOURCES := $(wildcard $(ROOT)/*.go $(ROOT)/*.c $(ROOT)/*.h) \
           $(wildcard $(ROOT)/internal/core/*.go $(ROOT)/golib/*.go) \
           $(ROOT)/go.mod $(ROOT)/go.sum

.PHONY: test test-unit test-links test-metrics test-c test-maunet test-subs test-stress \
        test-agent test-e2e test-e2e-links test-e2e-subs \
        test-c-ping test-c-ping-local xray clean

test: test-unit test-links test-metrics test-c test-maunet test-subs test-e2e test-e2e-subs test-e2e-links

$(LIB): $(SOURCES)
	cd $(ROOT) && $(MISE) exec -- env CGO_ENABLED=1 CC=$(CGO_CC) $(GO) build \
		-p=1 -trimpath -buildmode=c-shared -ldflags "$(CORE_LDFLAGS)" \
		-o dist/libmewnet.so .

# CGO is needed to compile the root package (the cgo exports); the Go API in
# golib/ and the core in internal/core are pure Go.
test-unit: $(LIB)
	cd $(ROOT) && $(MISE) exec -- env CGO_ENABLED=1 CC=$(CGO_CC) $(GO) test ./...
	$(PYTHON) $(ROOT)/tests/python/test_api.py

test-links: $(LIB)
	$(PYTHON) $(ROOT)/tests/python/test_links.py

# TTFB (ping), TTLB and throughput through a local HTTP server: probe, link
# test and link ping all record the same metrics.
test-metrics: $(LIB)
	$(PYTHON) $(ROOT)/tests/python/test_metrics.py

test-stress: $(LIB)
	$(PYTHON) $(ROOT)/tests/python/test_stress.py

# mewnet C ABI.
test-c: $(LIB)
	$(CC) $(ROOT)/tests/c/test_api.c -I$(ROOT) -L$(ROOT)/dist -lmewnet \
		-Wl,--allow-shlib-undefined -Wl,-rpath,'$$ORIGIN' -o $(ROOT)/dist/test_api
	$(ROOT)/dist/test_api

# Global-state facade (maunet): link ids, default selection, subscriptions.
test-maunet: $(LIB)
	$(CC) $(ROOT)/tests/c/test_maunet.c -I$(ROOT) -L$(ROOT)/dist -lmewnet \
		-Wl,--allow-shlib-undefined -Wl,-rpath,'$$ORIGIN' -o $(ROOT)/dist/test_maunet
	$(ROOT)/dist/test_maunet

# Subscription internals: base64, SHA-256, rating, groups.
test-subs: $(LIB)
	for t in base64 sha256 rating groups metrics; do \
		$(CC) $(ROOT)/tests/subs/test_$$t.c -I$(ROOT) -L$(ROOT)/dist -lmewnet \
			-Wl,--allow-shlib-undefined -Wl,-rpath,'$$ORIGIN' \
			-o $(ROOT)/dist/test_$$t || exit 1; \
		$(ROOT)/dist/test_$$t || exit 1; \
	done

# Ping through the C ABI using a real link:
#   make test-c-ping MEWNET_TEST_VLESS='vless://...' [MEWNET_PING_URL=...]
test-c-ping: $(LIB)
	$(CC) $(ROOT)/tests/c/test_ping.c -I$(ROOT) -L$(ROOT)/dist -lmewnet \
		-Wl,--allow-shlib-undefined -Wl,-rpath,'$$ORIGIN' -o $(ROOT)/dist/test_ping
	@if [ -z "$(MEWNET_TEST_VLESS)" ]; then \
		echo "set MEWNET_TEST_VLESS='vless://...'"; exit 2; \
	fi
	$(ROOT)/dist/test_ping "$(MEWNET_TEST_VLESS)" "$(MEWNET_PING_URL)"

# Same, but against a local Xray VLESS server (no real link needed).
test-c-ping-local: $(LIB)
	$(CC) $(ROOT)/tests/c/test_ping.c -I$(ROOT) -L$(ROOT)/dist -lmewnet \
		-Wl,--allow-shlib-undefined -Wl,-rpath,'$$ORIGIN' -o $(ROOT)/dist/test_ping
	$(PYTHON) $(ROOT)/tests/e2e/c_ping_demo.py

# End-to-end: needs Xray (see the xray target) and the mewnet-subs client.
test-e2e: $(LIB)
	$(PYTHON) $(ROOT)/tests/e2e/run.py

test-e2e-links: $(LIB)
	$(PYTHON) $(ROOT)/tests/e2e/run_links.py

test-e2e-subs: $(LIB)
	$(MAKE) -C $(ROOT)/apps subs
	$(PYTHON) $(ROOT)/tests/e2e/run_subs.py

# Offline failover/recovery test for the discovery agent (CLI in --headless).
test-agent: $(LIB)
	$(MAKE) -C $(ROOT)/apps subs
	$(PYTHON) $(ROOT)/tests/e2e/run_gtk_agent.py

# Download the Xray binary used by the e2e tests (not committed).
xray:
	mkdir -p /tmp/opencode/xray
	cd /tmp/opencode/xray && curl -fsSL -o xray.zip \
		https://github.com/XTLS/Xray-core/releases/latest/download/Xray-linux-64.zip
	cd /tmp/opencode/xray && $(PYTHON) -c "import zipfile; zipfile.ZipFile('xray.zip').extractall('.')"
	chmod +x /tmp/opencode/xray/xray

clean:
	rm -rf $(ROOT)/dist
