# Ascolto architecture

Ascolto is a native C/GTK 3 Linux application. PipeWire capture, local Whistle inference, speaker clustering, and transcript/FLAC recording run behind the existing core interfaces. A Unix control socket exposes the same session operations to the CLI and autonomous harness.

## Desktop surfaces

The GNOME AppIndicator menu contains session controls, current status/timer, background-job activity, recent session folders, recap/speech actions, Settings, and Quit. Menu appearance belongs to GNOME. The dedicated GTK Settings window provides General, Transcription, Recording, Storage, OpenCode, OpenRouter, and OpenAI pages with Ubuntu typography and system/light/dark appearance.

The desktop launcher opens Settings. Explicit Start/Stop actions still operate the session. Closing Settings hides the window and leaves the daemon running. Settings are validated as a batch, saved, and deferred until recording closes. The main GTK thread applies them at the idle boundary; model-path changes reload Whistle there.

## Persistence

`~/.local/share/ascolto/ascolto.db` contains schema-versioned SQLite tables for preferences, sessions, speakers, segments, artifacts, and AI jobs. WAL and foreign keys are enabled. Provider and task-selection settings live in the preferences JSON; each job receives an immutable copy, rather than consulting mutable preferences during execution.

On first startup, existing INI preferences are imported without deleting the source. On later startups the database is authoritative. `--config <path>` explicitly imports that INI into an isolated `<path>.db`, useful for distinct profiles. Command-line overrides apply after loading. Saving Settings persists the resulting effective preference values.

Session folders remain on disk. While idle, the index discovers existing `call_*` folders and their artifacts and imports valid segment/speaker metadata idempotently. Legacy malformed JSONL lines are skipped without altering the source. Stable SQLite session IDs are independent of the process-local session counter. New output folders apply only to future sessions; existing folders are neither moved nor deleted.

## Background work

The C core supervises up to eight worker processes. SQLite prevents concurrent jobs for the same session. Each Python worker receives a job ID and reads its configuration snapshot. The daemon reaps workers, records success/failure, and emits lifecycle telemetry. Workers use dedicated process groups and a parent-death signal. Startup marks abandoned jobs interrupted; retry is explicit.

The pipeline supports optional OpenAI cloud retranscription, recap through Claude CLI/OpenCode CLI/OpenRouter, and optional OpenAI speech generation. Local Whistle remains the live transcription engine. Cloud transcription writes `transcript.cloud.md`; it does not replace `transcript.md` or delete `transcript.jsonl`. Generated artifacts are published through private temporary files and atomic rename. If a later stage fails, already published artifacts remain available and the job is marked failed.

Credential values are resolved at execution time from environment variables or 1Password references. Only references enter SQLite. OpenCode uses a dedicated agent, denied tools, disabled sharing, an isolated working directory, JSON event output, and `--pure`. Claude runs without tools or session persistence. Recap adapters receive locally scrubbed transcripts and a prompt that treats transcript instructions as data.

Provider contracts follow [OpenCode permissions](https://opencode.ai/docs/permissions/), [OpenCode configuration](https://opencode.ai/docs/config/), and the [OpenRouter chat completion API](https://openrouter.ai/docs/api/api-reference/chat/create-a-chat-completion). Connection tests are explicit user actions; readiness checks do not prove that every selected model is available.

## Control and diagnostics

Existing commands remain available. Added commands are `OPEN_SETTINGS`, `GET_SETTINGS`, `UPDATE_SETTINGS <json>`, `GET_PROVIDER_STATUS`, `LIST_SESSIONS`, `LIST_JOBS`, `RETRY_JOB <id>`, and `DIAGNOSTICS`. `STATUS` includes pending settings and active background-job count. `GET_EVENTS` supports a sequence cursor and reports dropped history.

The recording states remain `IDLE`, `RECORDING_AUTO`, `RECORDING_MANUAL`, and `PAUSED`. AI job state is separate. Test injection is available only with the mock audio backend. See [the test harness guide](TESTING.md) for evidence formats, isolation, fixtures, and reproducible commands.
