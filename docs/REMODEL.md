# Remodel implementation status

The approved remodel is implemented locally. The running desktop daemon has been restarted while idle and Settings has been opened. Runtime diagnostics confirm Whistle loaded, SQLite schema 1 available, and the native tray indicator active.

## Delivered

- Seven GTK Settings pages, Ubuntu typography, sage/green identity, light/dark themes, Apply/Revert and deferred session changes.
- Compact native tray, recent sessions, background activity, Settings activation, and a launcher that does not toggle recording.
- SQLite preferences, legacy import, session/artifact index, speaker/segment persistence, and supervised AI jobs with explicit retry.
- Claude CLI, OpenCode CLI, OpenRouter recap adapters, and configurable OpenAI transcription/speech.
- Original transcript preservation and atomic publication of generated artifacts.
- Autonomous component diagnostics, cursor-based events, multi-speaker fixtures, fault injection, and isolated end-to-end evidence.

## Verification

- 19 isolated provider/end-to-end tests passed; SQLite contract tests passed.
- 6 native tests passed, including real local Whistle inference, speaker re-identification, socket control, and FLAC round-trip.
- Native checks passed with AddressSanitizer and UndefinedBehaviorSanitizer; process-global library leak checking is excluded.
- Seven pages rendered in both themes. GTK tests verify navigation, Apply/Revert, hide/reopen, and AppIndicator creation/destruction on an isolated session bus.
- Local OpenCode resolved configuration confirmed denied tools and disabled sharing, without invoking a model.
- Build completed without compiler warnings; `git diff --check` passed.

Latest complete autonomous evidence: `build/test-runs/20261015-191934/`. UI evidence: `/tmp/ascolto-ui/`. Commands and report formats are documented in `TESTING.md`.

Paid provider requests and real microphone integration tests were not executed. Git publication is tracked separately from local runtime verification.
