import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch, Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import providers
import recap


class Providers(unittest.TestCase):
    def setUp(self):
        # Deny accidental real requests in every adapter test.
        self.network = patch("requests.sessions.Session.request", side_effect=AssertionError("Unexpected network request"))
        self.network.start()
        self.addCleanup(self.network.stop)

    def test_credential_references_and_redaction(self):
        with patch.dict(os.environ, {"TEST_KEY": "fixture-secret"}):
            self.assertEqual(providers.credential("env:TEST_KEY"), "fixture-secret")
        with self.assertRaises(RuntimeError):
            providers.credential("sk-not-a-reference")
        with patch("providers.subprocess.run", return_value=SimpleNamespace(returncode=1, stdout="", stderr="fixture-secret")):
            with self.assertRaises(RuntimeError) as error:
                providers.credential("op://test/provider/key")
        self.assertNotIn("fixture-secret", str(error.exception))

    def test_openrouter_success_and_http_error(self):
        config = {"openrouter_credential": "env:TEST_KEY"}
        response = Mock(status_code=200)
        response.json.return_value = {"choices": [{"message": {"content": "Recap text"}}]}
        with patch("providers.credential", return_value="fixture-secret"), patch("providers.requests.post", return_value=response) as post:
            self.assertEqual(providers.recap("openrouter", "fixture/model", "words", "instructions", config), ("Recap text", "fixture/model"))
            self.assertEqual(post.call_args.kwargs["json"]["messages"][1]["content"], "words")
            response.status_code = 401
            with self.assertRaisesRegex(RuntimeError, "HTTP 401"):
                providers.recap("openrouter", "fixture/model", "words", "instructions", config)

    def test_openrouter_malformed_response_and_timeout(self):
        response = Mock(status_code=200)
        response.json.return_value = {"choices": []}
        with patch("providers.credential", return_value="fixture-secret"), patch("providers.requests.post", return_value=response):
            with self.assertRaisesRegex(RuntimeError, "Malformed"):
                providers.recap("openrouter", "model", "words", "instructions", {"openrouter_credential": "env:TEST_KEY"})
        with patch("providers.credential", return_value="fixture-secret"), patch("providers.requests.post", side_effect=TimeoutError):
            with self.assertRaises(TimeoutError):
                providers.recap("openrouter", "model", "words", "instructions", {"openrouter_credential": "env:TEST_KEY"})

    def test_opencode_permissions_and_event_parser(self):
        events = '\n'.join(json.dumps({"type": "text", "part": {"text": text}}) for text in ("A ", "recap"))
        with patch("providers.opencode_ready", return_value="/fixture/opencode"), patch("providers.subprocess.run", return_value=SimpleNamespace(returncode=0, stdout=events)) as run:
            self.assertEqual(providers.recap("opencode", "fixture/model", "supplied text", "summarize", {"opencode_path": "opencode"}), ("A recap", "fixture/model"))
            call = run.call_args
            config = json.loads(call.kwargs["env"]["OPENCODE_CONFIG_CONTENT"])
            self.assertEqual(config["permission"], {"*": "deny"})
            self.assertEqual(config["share"], "disabled")
            self.assertIn("--pure", call.args[0])
            self.assertEqual(call.kwargs["input"], "supplied text")
            self.assertNotIn("supplied text", call.args[0])

    def test_opencode_missing_executable_malformed_and_failure(self):
        with patch("providers.shutil.which", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "not found"):
                providers.opencode_ready("missing")
        with patch("providers.opencode_ready", return_value="fixture"):
            for result in (SimpleNamespace(returncode=1, stdout=""), SimpleNamespace(returncode=0, stdout="not-json"),
                           SimpleNamespace(returncode=0, stdout='{"type":"error"}'), SimpleNamespace(returncode=0, stdout="")):
                with patch("providers.subprocess.run", return_value=result), self.assertRaises(RuntimeError):
                    providers.recap("opencode", "fixture/model", "words", "instructions", {"opencode_path": "fixture"})

    def test_openai_transcription_and_speech_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "input.wav"
            source.write_bytes(b"fixture")
            response = Mock(status_code=200)
            response.json.return_value = {"text": "Spoken words"}
            session = Mock()
            session.post.return_value = response
            self.assertEqual(recap.transcribe_chunk(session, "fixture", "model", "en", "", str(source)), "Spoken words")
            target = Path(directory) / "recap.mp3"
            with patch("recap.requests.post", return_value=Mock(status_code=200, content=b"fixture-mp3")):
                recap.tts_mp3("fixture", "Words", str(target), "en", "onyx")
            self.assertEqual(target.read_bytes(), b"fixture-mp3")
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)
            with patch("recap.requests.post", return_value=Mock(status_code=401, text="fixture-secret")):
                with self.assertRaisesRegex(RuntimeError, "HTTP 401"):
                    recap.tts_mp3("fixture", "Words", str(target), "en", "onyx")
            self.assertEqual(target.read_bytes(), b"fixture-mp3")

    def test_atomic_publication_failure_keeps_previous_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "recap.md"
            path.write_text("original")
            with patch("recap.os.replace", side_effect=OSError("disk failure")), self.assertRaises(OSError):
                recap.atomic_write(str(path), "replacement")
            self.assertEqual(path.read_text(), "original")
            self.assertEqual([p.name for p in Path(directory).iterdir()], ["recap.md"])

    def test_scrubbing_and_echo_removal(self):
        self.assertNotIn("someone@example.com", recap.scrub("Email someone@example.com"))
        mic = [{"start": 0, "end": 2, "text": "Our next task is testing"}]
        system = [{"start": 0, "end": 2, "text": "Our next task is testing"}]
        self.assertLess(len(recap.drop_echo(mic, system)), len(mic))
