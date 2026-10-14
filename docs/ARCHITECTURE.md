# Architecture and Implementation Plan

## 1. Target Environment Baseline

Inspected on host `voloire-XPS`:

- **OS**: Ubuntu 26.04.1 LTS (Resolute Raccoon), Kernel `7.0.0-38-generic` x86_64
- **CPU / Memory**: Intel Core i7-8550U (4 cores / 8 threads @ 1.80 GHz, AVX2), 14 GiB RAM
- **Desktop**: GNOME Shell 50.1 on Wayland (`ubuntu:GNOME`), extension `ubuntu-appindicators@ubuntu.com` enabled
- **Audio Stack**: PipeWire 1.6.2 + WirePlumber 1.6.2 (`pw-dump`, `pw-record`, `pw-play`, `wpctl`, `libpipewire-0.3`, `libpulse0`)
- **Toolchain & Dev Packages**: `gcc` 15.2.0, `make`, `pkg-config`, `libglib2.0-dev`, `libgio-2.0-dev`, `libjson-glib-dev`, `libavcodec-dev`, `libavformat-dev`, `libavutil-dev`, plus runtime libraries `libgtk-3-0t64` and `libayatana-appindicator3-1`

## 2. Modular Architecture

`ascolto` is structured around clean C vtable interfaces so that every subsystem can run against either production Linux backends or deterministic test-harness backends:

1. **`ascolto_core`**: finite state machine (`IDLE`, `RECORDING_AUTO`, `RECORDING_MANUAL`, `PAUSED`), session lifecycle, configuration persistence (`~/.config/ascolto/config.ini`), and transcript writer.
2. **`mic_watcher`**: monitors PipeWire / PulseAudio graph changes to detect active capture streams from third-party applications (excluding `ascolto` itself) with configurable start/stop debounce.
3. **`audio_capture`**: dual 16 kHz mono PCM capture streams:
   - `MIC` channel from the selected microphone source.
   - `SYS` channel from the selected sink monitor source.
4. **`whistle_engine`**: C wrapper around Cactus Compute Whistle (`needle.h`, `libneedle.a`, `whistle.cact`) providing streaming transcription, language selection, custom keyword boosting, and per-segment speech embedding extraction.
5. **`speaker_diarizer`**: deterministic `ME` assignment on `MIC` segments and online L2-normalized cosine similarity clustering with exponential moving average centroids on `SYS` embeddings to label `SPEAKER_1`, `SPEAKER_2`, ..., `SPEAKER_N`.
6. **`tray_ui`**: GNOME top bar integration over D-Bus (`org.kde.StatusNotifierItem` and `com.canonical.dbusmenu` / `libayatana-appindicator3`) displaying live status and timer (`MM:SS`) and exposing all configuration controls.
7. **`observability`**: structured JSONL event bus and Unix domain control socket (`/run/user/<uid>/ascolto.sock` or configurable path) used by `ascoltoctl` and automated tests.

## 3. Delivery Phases

1. **Phase 1 - Test Harness & Observability**: event bus, Unix control socket, mock audio/trigger/UI backends, multi-speaker call simulator, and automated unit + E2E test runner.
2. **Phase 2 - Whistle Engine & Speaker Diarizer**: integration with `needle.h` / `libneedle.a` / `whistle.cact`, dual-stream segment merger, and cosine speaker clustering.
3. **Phase 3 - PipeWire Audio & Auto-Trigger Backend**: live microphone activity detection and dual-channel capture (MIC + SYS monitor).
4. **Phase 4 - GNOME Top Bar Indicator & Configuration**: tray icon, live timer, menu items, custom keywords dialog, and transcript management.
5. **Phase 5 - End-to-End Verification**: full test suite execution and sanitizer checks.
