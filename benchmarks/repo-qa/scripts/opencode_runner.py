#!/usr/bin/env python3
"""Shared isolated OpenCode subprocess runner for the A/B harnesses.

`comprehension_ab.py` and `response_shape_ab.py` each ran `opencode run` with
their own environment-isolation, event-parsing, failure-classification, and
status-ladder copies, and the copies had drifted: comprehension cells redirected
only XDG_CONFIG_HOME, so the real user's opencode state/cache leaked into the
experiment. One runner now owns the full isolation contract:

- every XDG base directory points inside the harness workspace,
- shared model metadata and credentials are copied into the isolated dirs once
  per workspace (never printed, never persisted into records),
- the child runs in its own session and the whole process group is killed on
  timeout, so a wrapper timeout cannot leak grandchildren,
- provider-quota detection matches 429 on a token boundary instead of anywhere
  in the diagnostic blob.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path

import replay_common

_QUOTA_MARKERS = ("quota", "rate limit", "rate_limit", "too many requests")
_STATUS_429 = re.compile(r"(?<![\w.])429(?![\w.])")


def classify_failure(message: str) -> str | None:
    lowered = message.casefold()
    if any(marker in lowered for marker in _QUOTA_MARKERS) or _STATUS_429.search(lowered):
        return "provider_quota"
    return None


def parse_events(lines: list[str]) -> tuple[str, dict]:
    texts: dict[str, list[str]] = {}
    order: list[str] = []
    usage = {"input": 0, "output": 0, "reasoning": 0}
    provider_error = None
    malformed_event = False
    for line in lines:
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(event, dict):
            continue
        part = event.get("part", event)
        if not isinstance(part, dict):
            continue
        kind = part.get("type")
        if kind == "text":
            message = part.get("messageID", "?")
            text = part.get("text", "")
            if not isinstance(message, str) or not isinstance(text, str):
                malformed_event = True
                continue
            if message not in texts:
                texts[message] = []
                order.append(message)
            texts[message].append(text)
        elif kind == "step_finish":
            tokens = part.get("tokens") or {}
            if not isinstance(tokens, dict):
                tokens = {}
            for key in usage:
                value = tokens.get(key, 0) or 0
                if not isinstance(value, (int, float)) or isinstance(value, bool):
                    malformed_event = True
                    continue
                usage[key] += value
        elif kind == "error":
            provider_error = event.get("error") or part.get("error") or "provider error"
    return ("\n".join(texts[order[-1]]) if order else ""), {
        "tokens": usage, "provider_error": provider_error,
        "malformed_event": malformed_event,
    }


def empty_git_workspace(parent: Path) -> Path:
    """A committed, corpus-free git workspace with all tools/permissions denied."""
    workspace = Path(tempfile.mkdtemp(prefix="opencode-empty-", dir=parent))
    try:
        subprocess.run(["git", "init", "-q"], cwd=workspace, check=True)
        (workspace / ".gitignore").write_text("*\n!.gitignore\n!opencode.json\n")
        (workspace / "opencode.json").write_text(replay_common.pretty({
            "$schema": "https://opencode.ai/config.json",
            "mcp": {},
            "tools": {"*": False},
            "permission": {"*": "deny"},
        }))
        subprocess.run(["git", "add", ".gitignore", "opencode.json"], cwd=workspace, check=True)
        subprocess.run(
            ["git", "-c", "user.name=Harness", "-c", "user.email=harness@invalid", "commit", "-qm", "empty harness workspace"],
            cwd=workspace, check=True,
        )
        return workspace
    except BaseException:
        shutil.rmtree(workspace)
        raise


def isolated_environment(workspace: Path) -> dict[str, str]:
    """Environment that keeps ALL opencode state inside the workspace.

    Redirecting only XDG_CONFIG_HOME leaves the user's real state, data, and
    cache in play; every base directory must move for the run to be hermetic.
    Shared model metadata and the auth credential are copied into the isolated
    dirs exactly once per workspace so the run still authenticates without
    touching (or mutating) the user's real directories mid-run.
    """
    environment = os.environ.copy()
    environment["OPENCODE_CONFIG"] = str(workspace / "opencode.json")
    for variable in ("XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
        environment[variable] = str(workspace / f".{variable.replace('_', '-').lower()}")
    isolated_cache = Path(environment["XDG_CACHE_HOME"]) / "opencode"
    isolated_data = Path(environment["XDG_DATA_HOME"]) / "opencode"
    isolated_cache.mkdir(parents=True, exist_ok=True)
    isolated_data.mkdir(parents=True, exist_ok=True)
    shared_cache = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "opencode" / "models.json"
    shared_auth = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "opencode" / "auth.json"
    if shared_cache.exists() and not (isolated_cache / "models.json").exists():
        shutil.copy2(shared_cache, isolated_cache / "models.json")
    if shared_auth.exists() and not (isolated_data / "auth.json").exists():
        shutil.copy2(shared_auth, isolated_data / "auth.json")
    return environment


def run_opencode(executable: str, workspace: Path, model: str, prompt: str, timeout: float) -> dict:
    """Run one isolated `opencode run` cell and classify its outcome.

    Status ladder (shared verbatim by both A/B harnesses): malformed stream,
    quota, provider error, wrapper timeout (rc 124), other nonzero rc, empty
    answer, answered.
    """
    started = time.monotonic()
    environment = isolated_environment(workspace)
    process = subprocess.Popen(
        [executable, "run", "--format", "json", "-m", model, prompt],
        cwd=workspace, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        start_new_session=True,
    )
    timed_out = False
    try:
        raw_stdout, raw_stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        # kill the whole session: subprocess timeouts only kill the direct
        # child, leaving any spawned provider helpers running.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        raw_stdout, raw_stderr = process.communicate()
    answer, metadata = parse_events((raw_stdout or "").splitlines())
    if timed_out:
        status = "provider_timeout"
    else:
        failure_text = json.dumps(metadata["provider_error"]) + "\n" + (raw_stderr or "")
        classified = classify_failure(failure_text)
        if metadata["malformed_event"]:
            status = "malformed_provider_stream"
        elif classified:
            status = classified
        elif metadata["provider_error"]:
            status = "provider_error"
        elif process.returncode == 124:
            status = "provider_timeout"
        elif process.returncode != 0:
            status = f"exit_{process.returncode}"
        elif not answer.strip():
            status = "empty_answer"
        else:
            status = "answered"
    return {"status": status, "answer": answer,
            "raw_stdout": raw_stdout or "", "raw_stderr": raw_stderr or "",
            "wall_seconds": round(time.monotonic() - started, 3), **metadata}
