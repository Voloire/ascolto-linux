import json
import os
import sqlite3
import unittest
import wave
from pathlib import Path
from harness import Daemon, ROOT


class EndToEnd(unittest.TestCase):
    def setUp(self):
        self.directory = Path(os.environ["ASCOLTO_TEST_RUN"]) / self._testMethodName
        self.daemon = Daemon(self.directory)
        self.addCleanup(self.daemon.stop)
        self.daemon.start()

    def test_component_visibility_and_framed_commands(self):
        d = self.daemon
        diag = d.command("DIAGNOSTICS", fragments=True)
        self.assertEqual(set(diag) - {"ok", "schema_version", "monotonic_ms"},
                         {"core", "audio", "transcription", "diarization", "artifacts", "providers", "storage", "ui", "observability"})
        self.assertEqual(diag["audio"]["backend"], "mock")
        self.assertEqual(diag["transcription"]["backend"], "fixture")
        self.assertTrue(diag["storage"]["available"])
        self.assertTrue(diag["observability"]["telemetry_file_open"])
        self.assertEqual(self.daemon.socket.stat().st_mode & 0o777, 0o600)
        self.assertGreaterEqual(len(d.command("LIST_DEVICES")["devices"]), 2)
        self.assertFalse(d.command("UNKNOWN_COMMAND")["ok"])
        self.assertFalse(d.command("INJECT_SYNTH_SPEECH BAD 100 400 1000 1000 invalid")["ok"])
        self.assertFalse(d.command("INJECT_SYNTH_SPEECH MIC nan 400 1000 1000 invalid")["ok"])

    def test_multispeaker_audio_files_and_diarization(self):
        d = self.daemon
        d.settings(save_audio=True)
        d.settled()
        self.assertEqual(d.command("SIM_MIC_ACTIVE 1 FixtureCall")["state"], "RECORDING_AUTO")
        scenario = d.scenario(ROOT / "tests/scenarios/architecture_call.json")
        segments = d.command("LIST_SEGMENTS")["segments"]
        self.assertEqual(len(segments), len(scenario["turns"]))
        self.assertEqual([s["speaker"] for s in segments], [scenario["participants"][t["participant"]]["speaker"] for t in scenario["turns"]])
        diag = d.command("DIAGNOSTICS")
        self.assertGreater(diag["audio"]["MIC"]["accepted_samples"], 0)
        self.assertTrue(diag["audio"]["SYS"]["recorder_open"])
        directory = Path(diag["artifacts"]["session_dir"])
        self.assertEqual(d.command("SIM_MIC_ACTIVE 0 FixtureCall")["state"], "IDLE")
        records = [json.loads(line) for line in (directory / "transcript.jsonl").read_text().splitlines()]
        self.assertEqual([r["text"] for r in records], [t["text"] for t in scenario["turns"]])
        for channel in ("mic", "sys"):
            self.assertEqual((directory / f"{channel}.flac").read_bytes()[:4], b"fLaC")
        d.wait(lambda: len(d.command("LIST_SESSIONS")) == 1)
        with sqlite3.connect(d.database) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM segments").fetchone()[0], len(scenario["turns"]))
            self.assertEqual(db.execute("SELECT count(*) FROM speakers").fetchone()[0], 3)
        events = [json.loads(line) for line in d.telemetry.read_text().splitlines()]
        self.assertIn("session_stopped", [e["event"] for e in events])
        self.assertEqual([e["seq"] for e in events], sorted({e["seq"] for e in events}))

    def test_pause_silence_and_session_boundaries(self):
        d = self.daemon
        d.command("START")
        d.command("INJECT_SYNTH_SPEECH MIC 135 500 1500 1000 First turn")
        d.command("PAUSE")
        count = d.command("DIAGNOSTICS")["transcription"]["segments_total"]
        d.command("INJECT_SYNTH_SPEECH MIC 135 500 1500 1000 Must not be transcribed")
        diag = d.command("DIAGNOSTICS")
        self.assertEqual(diag["transcription"]["segments_total"], count)
        self.assertEqual(diag["audio"]["MIC"]["dropped_samples"], 16000)
        self.assertTrue(d.command("GET_TRAY_STATE")["paused"])
        d.command("RESUME")
        silence = self.directory / "silence.wav"
        with wave.open(str(silence), "wb") as wav:
            wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(16000); wav.writeframes(b"\0\0" * 16000)
        d.command(f"INJECT_WAV SYS {silence}")
        self.assertEqual(d.command("DIAGNOSTICS")["transcription"]["segments_total"], count)
        d.command("STOP")
        first = d.command("STATUS")["current_md_path"]
        d.command("START"); d.command("STOP")
        self.assertNotEqual(first, d.command("STATUS")["current_md_path"])
        self.assertTrue(Path(first).exists())

    def test_atomic_settings_pending_and_restart(self):
        d = self.daemon
        initial = d.command("GET_SETTINGS")
        response = d.command('UPDATE_SETTINGS {"language":"de","openai_credential":"sk-do-not-store"}')
        self.assertFalse(response["ok"])
        self.assertEqual(d.command("GET_SETTINGS"), initial)
        d.command("START")
        d.settings(language="en", mic_device="usb_headset_mic")
        self.assertTrue(d.command("DIAGNOSTICS")["core"]["settings_pending"])
        self.assertEqual(d.command("STATUS")["language"], initial["language"])
        d.command("STOP"); d.settled(); d.stop(); d.start()
        self.assertEqual(d.command("GET_SETTINGS")["language"], "en")
        self.assertEqual(d.command("STATUS")["mic_device"], "usb_headset_mic")
        self.assertNotIn(b"sk-do-not-store", d.database.read_bytes())

    def test_supervised_recap_failure_retry_and_preservation(self):
        d = self.daemon
        d.settings(cloud_transcription=False, recap_model="fixture-failure")
        d.settled(); d.command("START")
        d.command("INJECT_SYNTH_SPEECH MIC 135 500 1500 1000 Preserve our original words")
        d.command("STOP")
        md = Path(d.command("STATUS")["current_md_path"])
        original = md.read_bytes()
        self.assertTrue(d.command("RECAP")["ok"])
        d.wait(lambda: d.command("LIST_JOBS")[0]["state"] == "failed")
        self.assertEqual(md.read_bytes(), original)
        d.settings(recap_model="fixture-slow"); d.settled()
        failed_id=d.command("LIST_JOBS")[0]["id"]
        self.assertTrue(d.command(f"RETRY_JOB {failed_id}")["ok"])
        self.assertFalse(d.command("RECAP")["ok"])
        d.wait(lambda: d.command("LIST_JOBS")[0]["state"] == "succeeded")
        self.assertEqual(md.read_bytes(), original)
        self.assertTrue(md.with_name("transcript.jsonl").exists())
        self.assertIn("Preserve the live transcript", md.with_name("recap.md").read_text())
        with sqlite3.connect(d.database) as db:
            snapshot = json.loads(db.execute("SELECT snapshot FROM jobs ORDER BY id DESC LIMIT 1").fetchone()[0])
        self.assertEqual(snapshot["recap_model"], "fixture-slow")

    def test_event_cursor_and_ring_overflow(self):
        d = self.daemon
        for i in range(270):
            d.command(f"SET_KEYWORDS fixture-{i}")
        page = d.command("GET_EVENTS 0")
        self.assertGreater(page["lost_events"], 0)
        events = list(page["events"])
        while page["has_more"]:
            page = d.command(f"GET_EVENTS {page['next_after']}")
            events.extend(page["events"])
        self.assertEqual(len(events), 256)
        self.assertEqual(len({e["seq"] for e in events}), 256)
        self.assertEqual(d.command(f"GET_EVENTS {page['next_after']}")["events"], [])

    def test_interrupted_jobs_and_unknown_schema(self):
        d = self.daemon
        d.stop()
        with sqlite3.connect(d.database) as db:
            db.execute("INSERT INTO jobs(dir,kind,state,snapshot) VALUES('fixture','recap','running','{}')")
        d.start()
        self.assertEqual(d.command("LIST_JOBS")[0]["state"], "interrupted")
        d.stop()
        with sqlite3.connect(d.database) as db:
            db.execute("PRAGMA user_version=999")
        with self.assertRaises(RuntimeError):
            d.start()

    def test_unwritable_storage_fails_without_recording(self):
        d = self.daemon
        d.settings(output_dir="/proc/ascolto-test-no-write")
        d.settled()
        self.assertFalse(d.command("START")["ok"])
        diagnostic = d.command("DIAGNOSTICS")
        self.assertEqual(diagnostic["core"]["state"], "IDLE")
        self.assertFalse(diagnostic["artifacts"]["markdown_open"])
        self.assertIn("storage_error", [event["event"] for event in d.command("GET_EVENTS")["events"]])

    def test_duplicate_daemon_does_not_replace_socket(self):
        import subprocess
        d = self.daemon
        duplicate = subprocess.run([str(ROOT / "bin/ascolto"), "--headless", "--mock-audio", "--mock-engine",
                                    "--socket", str(d.socket)], env=d.env, cwd=ROOT, capture_output=True, timeout=5)
        self.assertNotEqual(duplicate.returncode, 0)
        self.assertEqual(d.command("STATUS")["state"], "IDLE")

    def test_capture_failure_is_visible_and_recoverable(self):
        d = self.daemon
        d.command("SIM_CAPTURE_FAILURE on")
        self.assertFalse(d.command("START")["ok"])
        self.assertEqual(d.command("STATUS")["state"], "IDLE")
        self.assertIn("audio_error", [e["event"] for e in d.command("GET_EVENTS")["events"]])
        d.command("SIM_CAPTURE_FAILURE off")
        self.assertTrue(d.command("START")["ok"])
        self.assertEqual(d.command("STATUS")["state"], "RECORDING_MANUAL")
        d.command("STOP")

    def test_parent_crash_terminates_worker_and_marks_interrupted(self):
        d = self.daemon
        d.settings(cloud_transcription=False, recap_model="fixture-crash")
        d.settled(); d.command("START")
        d.command("INJECT_SYNTH_SPEECH MIC 135 500 1500 1000 Crash recovery fixture")
        d.command("STOP"); d.command("RECAP")
        workers = d.command("DIAGNOSTICS")["providers"]["workers"]
        self.assertEqual(len(workers), 1)
        pid = workers[0]["pid"]
        d.stop(crash=True)
        def terminated():
            try:
                return Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1].startswith("Z")
            except FileNotFoundError:
                return True
        d.wait(terminated)
        d.start()
        self.assertEqual(d.command("LIST_JOBS")[0]["state"], "interrupted")
