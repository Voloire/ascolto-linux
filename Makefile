CC       := gcc
PKG_CFG  := pkg-config
GLIB_FLAGS := $(shell $(PKG_CFG) --cflags json-glib-1.0 gio-2.0 gobject-2.0 glib-2.0)
GLIB_LIBS  := $(shell $(PKG_CFG) --libs json-glib-1.0 gio-2.0 gobject-2.0 glib-2.0)
CFLAGS   := -std=c11 -Wall -Wextra -O2 -g -Iinclude -Ithird_party/whistle $(GLIB_FLAGS)
LDFLAGS  := third_party/whistle/libneedle.so -Wl,-rpath,'$$ORIGIN/../third_party/whistle' $(GLIB_LIBS) -ldl -lpthread -lm

CORE_SRCS := \
	src/types_config.c \
	src/observability.c \
	src/whistle_engine.c \
	src/diarizer.c \
	src/audio_backend.c \
	src/tray_ui.c \
	src/core.c

CORE_OBJS := $(patsubst src/%.c,build/%.o,$(CORE_SRCS))

.PHONY: all deps test install-desktop clean

all: deps bin/ascolto bin/ascoltoctl bin/test_harness

deps:
	@./scripts/fetch_whistle.sh

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

bin/ascolto: $(CORE_OBJS) src/main.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(CORE_OBJS) src/main.c -o $@ $(LDFLAGS)

bin/ascoltoctl: tools/ascoltoctl.c
	@mkdir -p bin
	$(CC) -std=c11 -Wall -Wextra -O2 -g tools/ascoltoctl.c -o $@

bin/test_harness: $(CORE_OBJS) tests/test_harness.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(CORE_OBJS) tests/test_harness.c -o $@ $(LDFLAGS)

test: all
	./bin/test_harness

install-desktop: all
	chmod +x scripts/launch_ascolto.sh
	mkdir -p $(HOME)/Desktop $(HOME)/.local/share/applications
	cp assets/ascolto.desktop $(HOME)/Desktop/ascolto.desktop
	cp assets/ascolto-daemon.desktop $(HOME)/Desktop/ascolto-daemon.desktop
	cp assets/ascolto.desktop $(HOME)/.local/share/applications/ascolto.desktop
	cp assets/ascolto-daemon.desktop $(HOME)/.local/share/applications/ascolto-daemon.desktop
	chmod +x $(HOME)/Desktop/ascolto.desktop $(HOME)/Desktop/ascolto-daemon.desktop
	chmod +x $(HOME)/.local/share/applications/ascolto.desktop $(HOME)/.local/share/applications/ascolto-daemon.desktop
	gio set $(HOME)/Desktop/ascolto.desktop metadata::trusted true 2>/dev/null || true
	gio set $(HOME)/Desktop/ascolto-daemon.desktop metadata::trusted true 2>/dev/null || true

clean:
	rm -rf build bin
