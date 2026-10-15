CC       := gcc
PKG_CFG  := pkg-config
GLIB_FLAGS := $(shell $(PKG_CFG) --cflags json-glib-1.0 gio-2.0 gobject-2.0 glib-2.0 gtk+-3.0 sqlite3)
GLIB_LIBS  := $(shell $(PKG_CFG) --libs json-glib-1.0 gio-2.0 gobject-2.0 glib-2.0 gtk+-3.0 sqlite3)
AV_FLAGS   := $(shell $(PKG_CFG) --cflags libavformat libavcodec libavutil)
AV_LIBS    := $(shell $(PKG_CFG) --libs libavformat libavcodec libavutil)
CFLAGS   := -std=c11 -Wall -Wextra -O2 -g -MMD -MP -Iinclude -Ithird_party/whistle $(GLIB_FLAGS) $(AV_FLAGS)
LDFLAGS  := third_party/whistle/libneedle.so -Wl,-rpath,'$$ORIGIN/../third_party/whistle' $(GLIB_LIBS) $(AV_LIBS) -ldl -lpthread -lm

CORE_SRCS := \
	src/types_config.c \
	src/settings.c \
	src/settings_ui.c \
	src/observability.c \
	src/diagnostics.c \
	src/whistle_engine.c \
	src/diarizer.c \
	src/audio_backend.c \
	src/audio_recorder.c \
	src/tray_ui.c \
	src/core.c

CORE_OBJS := $(patsubst src/%.c,build/%.o,$(CORE_SRCS))

.PHONY: all deps test install-desktop install-autostart clean

all: deps bin/ascolto bin/ascoltoctl bin/flac2wav bin/test_harness

deps:
	@./scripts/fetch_whistle.sh

-include $(CORE_OBJS:.o=.d)

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

bin/ascolto: $(CORE_OBJS) src/main.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(CORE_OBJS) src/main.c -o $@ $(LDFLAGS)

bin/ascoltoctl: tools/ascoltoctl.c
	@mkdir -p bin
	$(CC) -std=c11 -Wall -Wextra -O2 -g tools/ascoltoctl.c -o $@

bin/flac2wav: tools/flac2wav.c
	@mkdir -p bin
	$(CC) -std=c11 -Wall -Wextra -O2 -g $(AV_FLAGS) tools/flac2wav.c -o $@ $(AV_LIBS)

bin/test_harness: $(CORE_OBJS) tests/test_harness.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(CORE_OBJS) tests/test_harness.c -o $@ $(LDFLAGS)

test: bin/ascolto bin/ascoltoctl bin/flac2wav bin/test_settings
	./bin/test_settings
	python3 tests/run_harness.py

.PHONY: test-native test-ui
test-native: all
	./bin/test_harness

test-ui: bin/test_ui
	GIO_USE_VFS=local NO_AT_BRIDGE=1 GDK_BACKEND=x11 GDK_SCALE=1 GDK_DPI_SCALE=1 xvfb-run -a -s '-screen 0 1280x900x24' dbus-run-session -- ./bin/test_ui

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

install-autostart: all
	chmod +x scripts/launch_ascolto.sh
	mkdir -p $(HOME)/.config/autostart
	sed -e 's|^Actions=.*||' -e '/^\[Desktop Action/,$$d' assets/ascolto-daemon.desktop > $(HOME)/.config/autostart/ascolto-daemon.desktop
	printf 'X-GNOME-Autostart-enabled=true\nX-GNOME-Autostart-Delay=5\n' >> $(HOME)/.config/autostart/ascolto-daemon.desktop

clean:
	rm -rf build bin

bin/test_settings: build/settings.o build/types_config.o tests/test_settings.c
	$(CC) $(CFLAGS) $^ -o $@ $(GLIB_LIBS)

bin/test_ui: $(CORE_OBJS) tests/test_ui.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

.PHONY: test-remodel
test-remodel: bin/test_settings bin/test_ui
	./bin/test_settings
	python3 -m unittest discover -s tests -p 'test_providers.py'
	GIO_USE_VFS=local NO_AT_BRIDGE=1 GDK_BACKEND=x11 GDK_SCALE=1 GDK_DPI_SCALE=1 xvfb-run -a dbus-run-session -- ./bin/test_ui

.PHONY: test-live
test-live: all
	./bin/test_harness --live

bin/test_harness_sanitize: $(CORE_SRCS) tests/test_harness.c
	$(CC) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $^ -o $@ $(LDFLAGS)

.PHONY: test-sanitize
test-sanitize: bin/ascolto bin/ascoltoctl bin/test_harness_sanitize
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./bin/test_harness_sanitize
