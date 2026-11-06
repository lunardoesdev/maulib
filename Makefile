# mewnet — build and test

MISE    ?= mise
GO      := $(MISE) exec -- go
CC      := $(MISE) exec -- clang
PYTHON  ?= python3

X11_LIB  := $(shell ldconfig -p 2>/dev/null | awk '/libX11.so.6 /{print $$NF; exit}')
XCB_LIB  := $(shell ldconfig -p 2>/dev/null | awk '/libxcb.so.1 /{print $$NF; exit}')
XAU_LIB  := $(shell ldconfig -p 2>/dev/null | awk '/libXau.so.6 /{print $$NF; exit}')

MINGW_CC     := x86_64-w64-mingw32-gcc
MINGW_DLLTOOL:= x86_64-w64-mingw32-dlltool
MINGW_OBJDUMP:= x86_64-w64-mingw32-objdump

export GOPATH  ?= $(HOME)/.cache/mewnet/gopath
export GOCACHE ?= $(HOME)/.cache/mewnet/gocache
export CGO_ENABLED = 1

LDFLAGS = -s -w -X github.com/metacubex/mihomo/constant.Version=v1.19.33

.PHONY: all deps build build-mingw-core subs subs-mingw gui gtk watch test test-subs test-e2e-subs test-unit test-links test-stress test-c test-c-ping test-c-ping-local test-e2e test-e2e-links test-gtk-agent clean xray

all: build

deps:
	$(MISE) install

build:
	mkdir -p dist
	cd libs/mewnet && $(GO) build -p=2 -trimpath -buildmode=c-shared -ldflags "$(LDFLAGS)" -o ../../dist/libmewnet.so ./cmd/mewnet
	@ls -lh dist/libmewnet.so

build-mingw-core:
	mkdir -p dist/windows
	cd libs/mewnet && GOOS=windows GOARCH=amd64 CGO_ENABLED=1 CC=$(MINGW_CC) $(GO) build -p=2 -trimpath -buildmode=c-shared -ldflags "$(LDFLAGS)" -o ../../dist/windows/mewnet.dll ./cmd/mewnet
	{ echo 'LIBRARY mewnet.dll'; echo 'EXPORTS'; $(MINGW_OBJDUMP) -p dist/windows/mewnet.dll | grep -oE 'mewnet_[A-Za-z0-9_]+' | sort -u | sed 's/^/  /'; } > dist/windows/mewnet.def
	$(MINGW_DLLTOOL) -d dist/windows/mewnet.def -l dist/windows/libmewnet.dll.a -D mewnet.dll

subs-mingw: build-mingw-core
	$(MISE) exec -- cmake -S . -B build-mingw \
		-DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/cmake/toolchains/mingw-x86_64.cmake \
		-DCMAKE_BUILD_TYPE=Release \
		-DMEWNET_SUBS_BUILD_TESTS=OFF \
		-DMEWNET_BUILD_GTK=OFF \
		-DMEWNET_LIBRARY=$(CURDIR)/dist/windows/libmewnet.dll.a
	$(MISE) exec -- cmake --build build-mingw -j$(shell nproc)

test: test-unit test-links test-c test-subs test-e2e test-e2e-subs test-e2e-links

test-unit: build
	cd libs/mewnet && $(GO) test ./internal/...
	$(PYTHON) tests/python/test_api.py

test-links: build
	$(PYTHON) tests/python/test_links.py

subs: build
	$(MISE) exec -- cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang
	$(MISE) exec -- cmake --build build -j$(shell nproc)

test-subs: subs
	$(MISE) exec -- ctest --test-dir build --output-on-failure

test-e2e-subs: subs
	$(PYTHON) tests/e2e/run_subs.py

# Odin/raylib GUI. System libX11 has no unversioned dev symlink here, so the
# target creates a small local symlink directory for the linker.
WATCH_DB ?= data/mewnet-watch.db
WATCH_SUBS ?= https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub

watch: subs
	./build/apps/mewnet-watch/mewnet-watch --db $(WATCH_DB) --proxy-port 18085 $(WATCH_SUBS)

GTK_DB ?= data/mewnet-gtk.db

gtk: subs
	./build/apps/mewnet-gtk/mewnet-gtk --db $(GTK_DB)

# Offline failover/recovery test for the GTK discovery engine.
test-gtk-agent: subs
	$(PYTHON) tests/e2e/run_gtk_agent.py

gui: subs
	mkdir -p build/gui/syslib
	ln -sf "$(X11_LIB)" build/gui/syslib/libX11.so
	ln -sf "$(X11_LIB)" build/gui/syslib/libX11.so.6
	ln -sf "$(XCB_LIB)" build/gui/syslib/libxcb.so
	ln -sf "$(XCB_LIB)" build/gui/syslib/libxcb.so.1
	ln -sf "$(XAU_LIB)" build/gui/syslib/libXau.so
	ln -sf "$(XAU_LIB)" build/gui/syslib/libXau.so.6
	LIBRARY_PATH="$(CURDIR)/build/gui/syslib" $(MISE) exec -- odin build apps/gui \
		-out:build/mewnet-gui \
		-extra-linker-flags:"-L$(CURDIR)/build/libs/mewnet-subs -L$(CURDIR)/dist -lmewnet_subs -lmewnet -Wl,-rpath,$(CURDIR)/build/libs/mewnet-subs:$(CURDIR)/dist"

test-stress: build
	$(PYTHON) tests/python/test_stress.py

test-c: build
	$(CC) tests/c/test_api.c -Ilibs/mewnet/include -Ldist -lmewnet -Wl,-rpath,'$$ORIGIN' -o dist/test_api
	./dist/test_api

# Ping through the C ABI using a real link:
#   make test-c-ping MEWNET_TEST_VLESS='vless://...' [MEWNET_PING_URL='http://.../204']
test-c-ping: build
	$(CC) tests/c/test_ping.c -Ilibs/mewnet/include -Ldist -lmewnet -Wl,-rpath,'$$ORIGIN' -o dist/test_ping
	@if [ -z "$(MEWNET_TEST_VLESS)" ]; then \
		echo "set MEWNET_TEST_VLESS='vless://...'"; exit 2; \
	fi
	./dist/test_ping "$(MEWNET_TEST_VLESS)" "$(MEWNET_PING_URL)"

# Same, but against a local Xray VLESS server (no real link needed).
test-c-ping-local: build
	$(CC) tests/c/test_ping.c -Ilibs/mewnet/include -Ldist -lmewnet -Wl,-rpath,'$$ORIGIN' -o dist/test_ping
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
	rm -rf dist build build-mingw
