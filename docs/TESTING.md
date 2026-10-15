# Autonomous test harness

The default suite exercises the running application without opening real audio devices, resolving personal credentials, or calling paid providers. It covers the C core, Unix control protocol, transcript and FLAC writers, SQLite persistence, Python provider adapters, supervised workers, and GTK settings.

## Commands

```sh
make test          # SQLite contract tests + isolated provider/E2E suite
make test-native   # Real local Whistle model, diarization, WAV, FLAC and mock daemon
make test-ui       # GTK interactions and fourteen light/dark screenshots under Xvfb
make test-sanitize # Native checks under AddressSanitizer and UndefinedBehaviorSanitizer
```

`make test` does not download model artifacts. Run `make deps` explicitly if the Whistle library has not been installed. Python tests require `numpy` and `requests`. GTK rendering additionally requires Xvfb. ASan leak detection is disabled because the native model/GTK stack retains process-global allocations; invalid accesses and undefined behavior still terminate the run.

`make test-live` is a separate opt-in integration suite. It accesses the real PipeWire graph, starts real capture, and exercises desktop integration. It is not part of autonomous default testing. No automated suite makes paid provider calls.

## Isolation and evidence

The Python driver creates a unique home, database, output folder, Unix socket, and credential-free environment for every scenario. The daemon runs with `--headless --mock-audio --mock-engine`. Microphone events and speech are supplied through the normal control socket. A local fixture executable replaces Claude for worker lifecycle tests; HTTP adapters use mocked responses with an assertion that rejects unexpected network calls.

Every subprocess has a bounded startup, command, and shutdown timeout. Cleanup runs even when assertions fail. The crash scenario deliberately kills the parent and verifies that its supervised worker terminates. Interrupted work is recorded, never automatically submitted again.

Each run retains evidence under `build/test-runs/<timestamp>/`:

- `results.json` and `junit.xml`: test outcomes, durations, and failure traces.
- Per-scenario `commands.jsonl`: inputs, parsed responses, and command latency.
- `daemon.log` and `events.jsonl`: runtime diagnostics and structured lifecycle events.
- Isolated SQLite database and original/generated session artifacts.

Native checks use unique `/tmp/ascolto-native-*` folders. UI screenshots are written to `/tmp/ascolto-ui/`. Build output and test evidence are ignored by Git.

## Scenario coverage

| Component | Verification |
|---|---|
| Audio | Mock device enumeration, accepted/dropped sample counters, silence, dual channels, injected capture failure and recovery |
| Transcription | Deterministic fixture text; separately, real Whistle inference on local speech WAVs |
| Diarization | Local speaker plus two distinct remote voice profiles, repeat-speaker identity, channel separation |
| Session core | Automatic start/stop, manual sessions, pause/resume, ignored paused audio, independent session files |
| Artifacts | Valid escaped JSONL, retained original transcript, FLAC signatures and native sample-count round-trip, atomic publication failure |
| Settings | Batch validation, rejected secrets/types/unknown keys, deferred changes, restart persistence, Apply/Revert in GTK |
| SQLite | Import/reopen, idempotent session index, persisted segments/speakers, job exclusion, crash recovery, unsupported schema rejection |
| Providers | OpenAI transcription/speech, OpenRouter success/auth/malformed/timeout, OpenCode executable/options/event parsing, permission restrictions |
| Workers | Failure, explicit retry, duplicate suppression, immutable job snapshot, parent crash termination |
| Protocol | Fragmented frames, malformed commands, invalid/non-finite voice input, duplicate daemon exclusion, event cursor pagination and overflow |
| UI | Seven real GTK pages in both themes, sidebar navigation, edits, Apply/Revert, hide/reopen without daemon shutdown |

The reusable conversation is `tests/scenarios/architecture_call.json`: participants declare channel, vocal formants, and expected speaker identity; turns declare text and duration. The driver validates speaker assignment and exact artifact content. Simulated audio tests prove pipeline behavior, not recognition accuracy; the native suite checks the actual model separately.

## Observability contract

```sh
./bin/ascoltoctl DIAGNOSTICS
./bin/ascoltoctl STATUS
./bin/ascoltoctl GET_EVENTS 0
./bin/ascoltoctl LIST_DEVICES
./bin/ascoltoctl LIST_SPEAKERS
./bin/ascoltoctl LIST_SEGMENTS
./bin/ascoltoctl LIST_SESSIONS
./bin/ascoltoctl LIST_JOBS
./bin/ascoltoctl GET_PROVIDER_STATUS
```

`DIAGNOSTICS` schema version 1 reports core state, pending settings, audio backend and per-channel counters/RMS/VAD/recorder state, transcription backend/model/segment counts, diarization threshold and speakers, open artifacts, provider selection and active worker IDs/PIDs, SQLite availability/schema, tray mode, and event-buffer state. It does not resolve or return credentials.

`GET_EVENTS [after_seq]` returns `oldest_seq`, `latest_seq`, `lost_events`, `events`, `next_after`, and `has_more`. Continue from `next_after` when `has_more` is true. Sequence numbers are process-local. The bounded ring holds 256 events; the JSONL log preserves the complete emitted stream. Oversized or malformed legacy event payloads are explicitly flagged instead of breaking JSON framing.

Simulation commands require `--mock-audio`; production capture rejects them:

```text
SIM_MIC_ACTIVE 1 FixtureCall
SIM_CAPTURE_FAILURE on
SIM_CAPTURE_FAILURE off
INJECT_SYNTH_SPEECH MIC 135 500 1500 1000 Supplied fixture words
INJECT_SYNTH_SPEECH SYS 98 420 1180 1200 First remote speaker
INJECT_WAV SYS /absolute/path/to/fixture.wav
```

`--mock-engine` additionally disables real model loading and uses supplied fixture text. It is rejected without `--mock-audio`. Keep this distinction visible in reports: `transcription.backend` is `fixture` or `whistle`.
