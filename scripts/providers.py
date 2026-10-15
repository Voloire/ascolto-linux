"""Provider adapters. Credentials are resolved at execution time and never persisted."""
import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

import requests


def credential(reference):
    if reference.startswith("env:"):
        name = reference[4:]
        if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name):
            raise RuntimeError("Invalid environment credential reference")
        value = os.environ.get(name)
        if value:
            return value
        source = Path(os.environ.get("ASCOLTO_SECRETS_ENV", str(Path.home() / "dotfiles/secrets/secrets.env")))
        if source.exists():
            for line in source.read_text().splitlines():
                match = re.fullmatch(r"\s*" + re.escape(name) + r"=(op://\S+)\s*", line)
                if match:
                    return credential(match[1])
        raise RuntimeError("Credential unavailable; unlock 1Password or configure the environment")
    if reference.startswith("op://"):
        result = subprocess.run(["op", "read", "--no-newline", reference], capture_output=True, text=True, timeout=90)
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
        raise RuntimeError("1Password credential unavailable")
    raise RuntimeError("Use an env:NAME or op:// credential reference")


def opencode_ready(executable):
    path = shutil.which(executable)
    if not path:
        raise RuntimeError("OpenCode executable not found")
    result = subprocess.run([path, "run", "--help"], capture_output=True, text=True, timeout=15)
    if result.returncode or not all(flag in result.stdout for flag in ("--pure", "--format", "--agent", "--model")):
        raise RuntimeError("OpenCode version lacks required isolated execution options")
    return path


def recap(provider, model, prompt, system, config):
    if provider == "openrouter":
        key = credential(config["openrouter_credential"])
        response = requests.post("https://openrouter.ai/api/v1/chat/completions",
                                 headers={"Authorization": f"Bearer {key}"},
                                 json={"model": model, "messages": [{"role": "system", "content": system},
                                                                    {"role": "user", "content": prompt}]}, timeout=300)
        if response.status_code != 200:
            raise RuntimeError(f"OpenRouter HTTP {response.status_code}")
        try:
            text = response.json()["choices"][0]["message"]["content"]
        except (ValueError, KeyError, IndexError, TypeError):
            raise RuntimeError("Malformed OpenRouter response") from None
        if not isinstance(text, str) or not text.strip():
            raise RuntimeError("Empty OpenRouter response")
        return text.strip(), model
    if provider == "opencode":
        exe = opencode_ready(config["opencode_path"])
        if "/" not in model:
            raise RuntimeError("OpenCode model must be provider/model")
        env = os.environ.copy()
        # Runtime config has the highest precedence. A dedicated agent prevents
        # inherited build-agent permissions from enabling tools.
        env["OPENCODE_CONFIG_CONTENT"] = json.dumps({
            "share": "disabled", "autoupdate": False, "permission": {"*": "deny"},
            "agent": {"ascolto-recap": {"description": "Summarize a supplied transcript", "mode": "primary",
                                         "prompt": system, "permission": {"*": "deny"}, "tools": {"*": False}}}})
        env["OPENCODE_DISABLE_CLAUDE_CODE"] = "true"
        env["OPENCODE_DISABLE_AUTOUPDATE"] = "true"
        with tempfile.TemporaryDirectory(prefix="ascolto-recap-") as directory:
            result = subprocess.run([exe, "run", "--pure", "--format", "json", "--agent", "ascolto-recap", "--model", model],
                                    input=prompt, text=True, capture_output=True, cwd=directory, env=env, timeout=900)
        if result.returncode:
            raise RuntimeError("OpenCode recap failed; check local authentication and model")
        parts = []
        for line in result.stdout.splitlines():
            try:
                event = json.loads(line)
            except ValueError:
                raise RuntimeError("Malformed OpenCode event stream") from None
            if event.get("type") == "error":
                raise RuntimeError("OpenCode reported a provider error")
            if event.get("type") == "text":
                parts.append(event.get("part", {}).get("text", ""))
        text = "".join(parts).strip()
        if not text:
            raise RuntimeError("OpenCode returned no recap")
        return text, model
    raise RuntimeError("Unsupported recap provider")


def check(provider, reference, executable):
    if provider == "OpenCode":
        opencode_ready(executable)
        return "Executable ready. Authentication and model are verified when a job runs."
    key = credential(reference)
    url = "https://api.openai.com/v1/models" if provider == "OpenAI" else "https://openrouter.ai/api/v1/key"
    response = requests.get(url, headers={"Authorization": f"Bearer {key}"}, timeout=20)
    if response.status_code != 200:
        raise RuntimeError(f"Connection failed (HTTP {response.status_code})")
    return "Connection ready. Model availability is verified when a job runs."
