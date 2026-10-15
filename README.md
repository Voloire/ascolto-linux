# Ascolto

Native Linux call recording and transcription with local Whistle inference, separate microphone/system audio, speaker diarization, a GNOME tray menu, and a dedicated GTK Settings window.

## Build and run

Requirements: GCC, make, pkg-config, GTK 3 and SQLite development packages, GLib/GIO/json-glib, libavformat/libavcodec/libavutil, PipeWire, and Ayatana AppIndicator runtime. Python recap/testing requires `numpy` and `requests`. UI tests use Xvfb.

```sh
make deps
make -j4
./scripts/launch_ascolto.sh app
```

Opening the app presents Settings. Start recording from the tray or with `./bin/ascoltoctl START`. Closing Settings leaves the daemon running. `--headless` runs without desktop surfaces. `make install-desktop` installs the launchers; launch-at-login can be configured in General.

## Settings and providers

- **General:** automatic microphone activation, launch at login, system/light/dark appearance.
- **Transcription:** language, vocabulary, Whistle model, speaker sensitivity, optional cloud retranscription, recap provider/model/language, spoken recap.
- **Recording:** microphone, system audio source, FLAC recording.
- **Storage:** output directory, SQLite location, recent sessions and background jobs with retry.
- **OpenCode / OpenRouter / OpenAI:** executable or credential references, audio model/voice settings, explicit readiness/connection checks.

Whistle always handles live transcription. Recaps can use Claude CLI, the locally authenticated OpenCode CLI (`provider/model`), or OpenRouter. OpenAI handles cloud transcription and speech. Credentials use `env:NAME` or `op://vault/item/field`; raw keys are never saved. The existing OpenAI environment/1Password lookup remains supported.

Each call creates a folder under `~/Documents/Ascolto` by default. Original `transcript.md` and `transcript.jsonl` are preserved. Optional files include `mic.flac`, `sys.flac`, `transcript.cloud.md`, `recap.md`, `recap.spoken.txt`, and `recap.mp3`. Cloud retranscription requires recorded audio. Settings changed during recording take effect after that session closes.

The SQLite database imports the previous `~/.config/ascolto/config.ini` once and retains the original file. Explicit `--config <path>` profiles use `<path>.db`. Existing call folders are indexed without moving or rewriting files.

## Autonomous verification

```sh
make test          # Isolated component/provider/end-to-end tests, no paid calls
make test-native   # Real local Whistle and native audio-file verification
make test-ui       # GTK interactions and light/dark screenshots under Xvfb
make test-sanitize # ASan/UBSan native checks
```

The harness simulates three-person conversations, pause/resume, silence, audio failures, storage failures, provider errors, retries, and crash recovery. Every run retains command traces, telemetry, SQLite state, generated artifacts, JSON results, and JUnit XML under `build/test-runs/`. Real microphone/desktop checks require the separate `make test-live` target.

Read [TESTING.md](docs/TESTING.md) for the complete harness and observability contract, and [ARCHITECTURE.md](docs/ARCHITECTURE.md) for persistence, provider, and process boundaries.

## Control examples

```sh
./bin/ascoltoctl DIAGNOSTICS
./bin/ascoltoctl STATUS
./bin/ascoltoctl START
./bin/ascoltoctl PAUSE
./bin/ascoltoctl RESUME
./bin/ascoltoctl STOP
./bin/ascoltoctl OPEN_SETTINGS
./bin/ascoltoctl GET_SETTINGS
./bin/ascoltoctl UPDATE_SETTINGS '{"appearance":"dark"}'
./bin/ascoltoctl LIST_JOBS
./bin/ascoltoctl RETRY_JOB 1
./bin/ascoltoctl GET_EVENTS 0
```
