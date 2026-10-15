"""Bounded, isolated daemon driver used by autonomous end-to-end scenarios."""
import json
import os
import socket
import subprocess
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Daemon:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.home = self.directory / "home"
        (self.home / ".config/ascolto").mkdir(parents=True, exist_ok=True)
        self.calls = self.directory / "calls"
        self.socket = Path("/tmp") / ("ascolto-test-" + uuid.uuid4().hex[:12] + ".sock")
        self.telemetry = self.directory / "events.jsonl"
        self.trace = self.directory / "commands.jsonl"
        self.database = self.home / ".local/share/ascolto/ascolto.db"
        self.env = {"HOME": str(self.home), "PATH": "/usr/bin:/bin", "LANG": "C.UTF-8",
                    "XDG_CONFIG_HOME": str(self.home / ".config"), "XDG_DATA_HOME": str(self.home / ".local/share")}
        self.process = None
        self.log = None
        self.fixture_bin = self.directory / "fixture-bin"
        self.fixture_bin.mkdir(exist_ok=True)
        executable = self.fixture_bin / "claude"
        executable.write_text('''#!/usr/bin/python3
import sys, time
text = sys.stdin.read()
assert "--tools" in sys.argv and sys.argv[sys.argv.index("--tools")+1] == ""
assert "--no-session-persistence" in sys.argv
model = sys.argv[sys.argv.index("--model")+1]
if model == "fixture-failure": sys.exit(7)
if model == "fixture-slow": time.sleep(1)
if model == "fixture-crash": time.sleep(20)
print("## Decisions\\n- Preserve the live transcript.\\n---SPOKEN---\\nThe team agreed to preserve the live transcript.")
''')
        executable.chmod(0o700)
        self.env["PATH"] = f"{self.fixture_bin}:/usr/bin:/bin"

    def start(self):
        self.log = (self.directory / "daemon.log").open("a")
        self.process = subprocess.Popen([str(ROOT / "bin/ascolto"), "--headless", "--mock-audio", "--mock-engine",
                                         "--socket", str(self.socket), "--output-dir", str(self.calls),
                                         "--telemetry-log", str(self.telemetry)], cwd=ROOT, env=self.env,
                                        stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f"Daemon exited: {(self.directory / 'daemon.log').read_text()}")
            try:
                self.command("DIAGNOSTICS")
                return self
            except (OSError, ValueError):
                time.sleep(0.025)
        raise TimeoutError("Daemon socket did not become ready in 10 seconds")

    def command(self, command, fragments=False):
        start = time.monotonic()
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(10)
            client.connect(str(self.socket))
            data = (command + "\n").encode()
            if fragments:
                for i in range(0, len(data), 9):
                    client.sendall(data[i:i+9])
                    time.sleep(0.001)
            else:
                client.sendall(data)
            response = bytearray()
            while True:
                part = client.recv(65536)
                if not part:
                    break
                response.extend(part)
        result = json.loads(response)
        with self.trace.open("a") as stream:
            stream.write(json.dumps({"command": command, "elapsed_ms": (time.monotonic()-start)*1000,
                                     "response": result}) + "\n")
        return result

    def settings(self, **values):
        response = self.command("UPDATE_SETTINGS " + json.dumps(values))
        if not response["ok"]:
            raise AssertionError(response)
        return response

    def wait(self, predicate, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.025)
        raise TimeoutError("Expected condition not reached before deadline")

    def settled(self):
        return self.wait(lambda: not self.command("DIAGNOSTICS")["core"]["settings_pending"])

    def scenario(self, path):
        scenario = json.loads(Path(path).read_text())
        for turn in scenario["turns"]:
            who = scenario["participants"][turn["participant"]]
            f0, f1, f2 = who["voice"]
            result = self.command(f"INJECT_SYNTH_SPEECH {who['channel']} {f0} {f1} {f2} {turn['duration_ms']} {turn['text']}")
            if not result["ok"]:
                raise AssertionError(result)
        return scenario

    def stop(self, crash=False):
        if self.process is not None:
            if self.process.poll() is None:
                if crash:
                    self.process.kill()
                else:
                    try:
                        self.command("QUIT")
                    except OSError:
                        self.process.terminate()
                try:
                    self.process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=3)
                    raise
            if self.log:
                self.log.close()
            self.process = None
