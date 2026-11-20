# Optional clients built on top of libmewnet.
#
# The library itself is built from the repository root, see ../README.md:
#
#   go build -buildmode=c-shared -o dist/libmewnet.so .
#
# Everything here only needs dist/libmewnet.so and the headers in the root.

ROOT  := $(abspath $(CURDIR)/..)
BUILD := $(ROOT)/build
MISE  ?= mise

X11_LIB := $(shell ldconfig -p 2>/dev/null | awk '/libX11.so.6 /{print $$NF; exit}')
XCB_LIB := $(shell ldconfig -p 2>/dev/null | awk '/libxcb.so.1 /{print $$NF; exit}')
XAU_LIB := $(shell ldconfig -p 2>/dev/null | awk '/libXau.so.6 /{print $$NF; exit}')

MINGW_CC      := x86_64-w64-mingw32-gcc
MINGW_DLLTOOL := x86_64-w64-mingw32-dlltool
MINGW_OBJDUMP := x86_64-w64-mingw32-objdump

CORE_LDFLAGS = -s -w -X github.com/metacubex/mihomo/constant.Version=v1.19.33

WATCH_DB     ?= $(ROOT)/data/mewnet-watch.db
WATCH_SUBS   ?= https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub
DISCOVERY_DB ?= $(ROOT)/data/discovery.db
GTK_DB       ?= $(ROOT)/data/mewnet-gtk.db
CLI_DB       ?= $(ROOT)/data/mewnet-cli.db

.PHONY: all subs watch discovery gtk cli gui build-mingw-core subs-mingw clean

all: subs

# Build the C clients into build/apps/<name>/<name>.
subs:
	$(MISE) exec -- cmake -S $(CURDIR) -B $(BUILD)/apps \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang
	$(MISE) exec -- cmake --build $(BUILD)/apps -j1

watch: subs
	$(BUILD)/apps/mewnet-watch/mewnet-watch --db $(WATCH_DB) \
		--proxy-port 18085 $(WATCH_SUBS)

# Staged discovery probe: first N nodes fast, then the first hundred, then a
# slow background pass.
discovery: subs
	$(BUILD)/apps/mewnet-discovery/mewnet-discovery --db $(DISCOVERY_DB) \
		--fast 8 --bulk 100 --run-seconds 120 $(WATCH_SUBS)

gtk: subs
	$(BUILD)/apps/mewnet-gtk/mewnet-gtk --db $(GTK_DB)

cli: subs
	$(BUILD)/apps/mewnet-cli/mewnet-cli --db $(CLI_DB)

# Odin/raylib GUI. The system libX11 has no unversioned dev symlink here, so
# the target creates a small local symlink directory for the linker.
gui:
	mkdir -p $(BUILD)/gui/syslib
	ln -sf "$(X11_LIB)" $(BUILD)/gui/syslib/libX11.so
	ln -sf "$(X11_LIB)" $(BUILD)/gui/syslib/libX11.so.6
	ln -sf "$(XCB_LIB)" $(BUILD)/gui/syslib/libxcb.so
	ln -sf "$(XCB_LIB)" $(BUILD)/gui/syslib/libxcb.so.1
	ln -sf "$(XAU_LIB)" $(BUILD)/gui/syslib/libXau.so
	ln -sf "$(XAU_LIB)" $(BUILD)/gui/syslib/libXau.so.6
	LIBRARY_PATH="$(BUILD)/gui/syslib" $(MISE) exec -- odin build gui \
		-out:$(BUILD)/mewnet-gui \
		-extra-linker-flags:"-L$(ROOT)/dist -lmewnet -Wl,-rpath,$(ROOT)/dist"

# Windows build of the library, needed by the cross-built clients.
build-mingw-core:
	mkdir -p $(ROOT)/dist/windows
	cd $(ROOT) && GOOS=windows GOARCH=amd64 CGO_ENABLED=1 CC=$(MINGW_CC) \
		$(MISE) exec -- go build -p=1 -trimpath -buildmode=c-shared \
		-ldflags "$(CORE_LDFLAGS)" -o dist/windows/mewnet.dll .
	{ echo 'LIBRARY mewnet.dll'; echo 'EXPORTS'; \
	  $(MINGW_OBJDUMP) -p $(ROOT)/dist/windows/mewnet.dll \
	    | grep -oE 'mewnet_[A-Za-z0-9_]+' | sort -u | sed 's/^/  /'; } \
	  > $(ROOT)/dist/windows/mewnet.def
	$(MINGW_DLLTOOL) -d $(ROOT)/dist/windows/mewnet.def \
		-l $(ROOT)/dist/windows/libmewnet.dll.a -D mewnet.dll

subs-mingw: build-mingw-core
	$(MISE) exec -- cmake -S $(CURDIR) -B $(ROOT)/build-mingw/apps \
		-DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/cmake/toolchains/mingw-x86_64.cmake \
		-DCMAKE_BUILD_TYPE=Release \
		-DMEWNET_BUILD_GTK=OFF \
		-DMEWNET_LIBRARY=$(ROOT)/dist/windows/libmewnet.dll.a
	$(MISE) exec -- cmake --build $(ROOT)/build-mingw/apps -j1

clean:
	rm -rf $(BUILD) $(ROOT)/build-mingw
