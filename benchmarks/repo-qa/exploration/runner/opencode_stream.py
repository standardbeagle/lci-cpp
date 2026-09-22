"""opencode process + stream handling for `runner.adapter.OpencodeAdapter`.

Deliberately thin: the isolation contract, the token accounting, the answer
extraction and the failure ladder are REUSED from `scripts/opencode_runner`,
which the discovery harness already hardened over 160 executed cells. Only two
things are added here, both of which that module has no reason to carry:

  * the config file and PWD are aimed at a checkout the harness must not
    mutate, so `OPENCODE_CONFIG` moves OUT of the measured tree;
  * tool calls are extracted, because the exploration runner re-checks every
    emitted call against the arm allowlist (`runner.gate`).
"""

import json
import os
import signal
import subprocess
import sys
import time

_SCRIPTS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "scripts",
)
if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)

import opencode_runner  # noqa: E402  (path shim above is required first)


def isolated_environment(state_dir, *, config_path, pwd):
    """`opencode_runner.isolated_environment` with the config and cwd re-aimed.

    The upstream helper puts every XDG base dir AND the config inside one
    workspace. Here the workspace being measured is a pristine corpus
    checkout, so the config lives in `state_dir` with the rest of the state
    and only PWD points into the checkout.
    """
    from pathlib import Path

    environment = opencode_runner.isolated_environment(Path(state_dir))
    environment["OPENCODE_CONFIG"] = str(config_path)
    environment["PWD"] = str(pwd)
    return environment


def _tool_calls(lines):
    """Every tool invocation in the stream, in emission order, as
    (opencode_tool_id, arguments). Unparseable lines are skipped -- the
    answer/usage pass already flags a malformed stream."""
    calls = []
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(event, dict):
            continue
        kind = event.get("type")
        if isinstance(kind, str):
            kind = kind.replace("-", "_")
        if kind != "tool_use":
            continue
        part = event.get("part", {})
        if not isinstance(part, dict):
            continue
        state = part.get("state", {})
        arguments = state.get("input") if isinstance(state, dict) else None
        calls.append((part.get("tool", "?"),
                      arguments if isinstance(arguments, dict) else {}))
    return calls


# The runner's AgentResult taxonomy is narrower than opencode's ladder: it only
# distinguishes "the agent answered" from "it ran out of time" from "the
# provider failed". The precise opencode status is preserved in the transcript
# so a failed cell can still be attributed (rule 12: a provider failure is a
# named non-result, never a wrong answer).
_TIMEOUT_STATUSES = {"provider_timeout"}
_OK_STATUSES = {"answered"}


def run_cell(executable, checkout_dir, environment, model, prompt, timeout_seconds):
    started = time.monotonic()
    process = subprocess.Popen(
        [executable, "run", "--format", "json", "-m", model, prompt],
        cwd=checkout_dir, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        start_new_session=True,
    )
    timed_out = False
    try:
        raw_stdout, raw_stderr = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        raw_stdout, raw_stderr = process.communicate()

    lines = (raw_stdout or "").splitlines()
    answer, metadata = opencode_runner.parse_events(lines)
    if timed_out:
        status = "provider_timeout"
    else:
        failure_text = json.dumps(metadata["provider_error"]) + "\n" + (raw_stderr or "")
        classified = opencode_runner.classify_failure(failure_text)
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

    if status in _OK_STATUSES:
        hint = "ok"
    elif status in _TIMEOUT_STATUSES:
        hint = "timeout"
    else:
        hint = "provider_error"

    return {
        "status_hint": hint,
        "answer": answer,
        "tool_calls": _tool_calls(lines),
        "tokens": metadata["tokens"],
        "transcript": {
            "provider": "opencode",
            "model": model,
            "opencode_status": status,
            "wall_seconds": round(time.monotonic() - started, 3),
            "returncode": process.returncode,
            "stderr": (raw_stderr or "")[-4000:],
            "provider_error": metadata["provider_error"],
            # AgentResult carries only input and output tokens. Providers report
            # most of a long session's prompt as cache reads, so the full split
            # is kept here; without it the record under-counts usage and no
            # spend can be derived from it.
            "token_usage_full": dict(metadata["tokens"]),
        },
    }
