# milib — build and test

GO      ?= $(HOME)/.local/go/bin/go
CC      ?= cc
PYTHON  ?= python3

export GOPATH  ?= $(HOME)/.cache/milib/gopath
export GOCACHE ?= $(HOME)/.cache/milib/gocache
export CGO_ENABLED = 1

LDFLAGS = -s -w -X github.com/metacubex/mihomo/constant.Version=v1.19.33

.PHONY: all build test test-unit test-links test-stress test-c test-c-ping test-c-ping-local test-e2e test-e2e-links clean xray

all: build

build:
	mkdir -p dist
	$(GO) build -p=2 -trimpath -buildmode=c-shared -ldflags "$(LDFLAGS)" -o dist/libmilib.so ./cmd/milib
	@ls -lh dist/libmilib.so

test: test-unit test-links test-c test-e2e test-e2e-links

test-unit: build
	$(PYTHON) tests/python/test_api.py

test-links: build
	$(PYTHON) tests/python/test_links.py

test-stress: build
	$(PYTHON) tests/python/test_stress.py

test-c: build
	$(CC) tests/c/test_api.c -Iinclude -Ldist -lmilib -Wl,-rpath,'$$ORIGIN' -o dist/test_api
	./dist/test_api

# Ping through the C ABI using a real link:
#   make test-c-ping MILIB_TEST_VLESS='vless://...' [MILIB_PING_URL='http://.../204']
test-c-ping: build
	$(CC) tests/c/test_ping.c -Iinclude -Ldist -lmilib -Wl,-rpath,'$$ORIGIN' -o dist/test_ping
	@if [ -z "$(MILIB_TEST_VLESS)" ]; then \
		echo "set MILIB_TEST_VLESS='vless://...'"; exit 2; \
	fi
	./dist/test_ping "$(MILIB_TEST_VLESS)" "$(MILIB_PING_URL)"

# Same, but against a local Xray VLESS server (no real link needed).
test-c-ping-local: build
	$(CC) tests/c/test_ping.c -Iinclude -Ldist -lmilib -Wl,-rpath,'$$ORIGIN' -o dist/test_ping
	$(PYTHON) tests/e2e/c_ping_demo.py

test-e2e: build
	$(PYTHON) tests/e2e/run.py

test-e2e-links: build
	$(PYTHON) tests/e2e/run_links.py

# Download the Xray binary used by the e2e tests (not committed).
xray:
	mkdir -p /tmp/opencode/xray
	cd /tmp/opencode/xray && curl -fsSL -o xray.zip \
		https://github.com/XTLS/Xray-core/releases/latest/download/Xray-linux-64.zip
	cd /tmp/opencode/xray && python3 -c "import zipfile; zipfile.ZipFile('xray.zip').extractall('.')"
	chmod +x /tmp/opencode/xray/xray

clean:
	rm -rf dist
