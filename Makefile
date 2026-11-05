# milib — build and test

GO      ?= $(HOME)/.local/go/bin/go
CC      ?= cc
PYTHON  ?= python3

export GOPATH  ?= $(HOME)/.cache/milib/gopath
export GOCACHE ?= $(HOME)/.cache/milib/gocache
export CGO_ENABLED = 1

LDFLAGS = -s -w -X github.com/metacubex/mihomo/constant.Version=v1.19.33

.PHONY: all build test test-unit test-links test-stress test-c test-e2e test-e2e-links clean xray

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
