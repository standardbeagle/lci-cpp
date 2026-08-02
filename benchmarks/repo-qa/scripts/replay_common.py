#!/usr/bin/env python3
"""Shared primitives for the OpenCode api-replay harnesses.

`api_replay_ab.py`, `api_replay_format_exploration.py` and `api_replay_all_tools.py`
all canonicalize requests, digest them, locate the tool result under test, and
diff two requests to prove treatment isolation.  Those four operations decide
whether a replay result is admissible evidence, so they must be one
implementation: a divergence between copies is silently a different experiment.

The definitions here are the ones already exercised by the frozen
format-exploration records, so importing them changes no committed artifact.
"""
from __future__ import annotations

import hashlib
import json
import os
import tempfile
from pathlib import Path

# Request headers that may be persisted into fixtures and replayed verbatim.
# Credential-bearing headers are never in this set; they are injected at runtime
# by the provider adapter and never recorded.
SAFE_REQUEST_HEADERS = {
    "accept",
    "content-type",
    "user-agent",
    "x-opencode-client",
    "x-opencode-project",
    "x-opencode-request",
    "x-opencode-session",
}


def canonical(value: object) -> str:
    """Serialize to the harness's single canonical JSON form.

    `ensure_ascii=True` keeps the form pure ASCII so a digest is stable across
    any locale or filesystem encoding; `allow_nan=False` rejects non-JSON floats
    rather than emitting the `NaN` extension a provider would reject.
    """
    return json.dumps(value, ensure_ascii=True, allow_nan=False, sort_keys=True, separators=(",", ":"))


def pretty(value: object) -> str:
    """The harness's single human-readable JSON artifact form.

    Committed artifacts (variants, surfaces, captures, raw cells) all use this
    byte form; keeping one implementation stops the copies from drifting into
    silently different artifact encodings.
    """
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def digest(value: object) -> str:
    """Content digest of a value or an already-canonical string.

    Captured provider content can contain lone surrogates, which are legal in a
    Python `str` but not encodable as strict UTF-8.  `surrogatepass` digests
    them deterministically instead of aborting the replay on a data-dependent
    input; digests of well-formed text are unaffected.
    """
    text = value if isinstance(value, str) else canonical(value)
    return "sha256:" + hashlib.sha256(text.encode("utf-8", errors="surrogatepass")).hexdigest()


def normalize_term(value: str) -> str:
    """Whitespace/case-insensitive comparison form for graded answer terms."""
    return " ".join(value.casefold().split())


def set_precision_recall(predicted: set, truth: set) -> tuple[float, float]:
    """Harness-wide IR convention for scoring a predicted set against truth.

    - both empty -> precision 1.0, recall 1.0 (nothing to find, nothing claimed)
    - empty predicted, nonempty truth -> precision defined as 0.0 so silence
      cannot score better than an attempt
    - nonempty predicted, empty truth -> recall defined as 0.0 so fabrication
      cannot score better than correctly saying nothing
    """
    true_positive = len(predicted & truth)
    precision = true_positive / len(predicted) if predicted else (1.0 if not truth else 0.0)
    recall = true_positive / len(truth) if truth else (1.0 if not predicted else 0.0)
    return precision, recall


def grade_sets(predicted: set, truth: set) -> dict:
    """Full IR scorecard under the shared convention, ready to embed in records."""
    precision, recall = set_precision_recall(predicted, truth)
    return {
        "exact": predicted == truth,
        "precision": precision,
        "recall": recall,
        "false_positives": sorted(predicted - truth),
        "false_negatives": sorted(truth - predicted),
    }


def write_atomic(path: Path, text: str) -> None:
    """Replace `path` with `text` atomically.

    Serialization stays at the call site (each artifact family has its own
    frozen byte form); this owns only the durability contract: write to a
    same-directory temp file, fsync, rename over the target, and unlink the
    temp file on any failure so an interrupted run leaves no debris — and
    never masks the original exception with an unlink error.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name, suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def write_immutable(path: Path, text: str) -> None:
    """Create `path` exactly once; refuse to overwrite an existing record."""
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    except FileExistsError as error:
        raise RuntimeError(f"refusing to replace immutable record: {path}") from error
    with os.fdopen(descriptor, "wb") as handle:
        handle.write(text.encode())
        handle.flush()
        os.fsync(handle.fileno())


def _escape_pointer_token(key: object) -> str:
    """Escape an object key per RFC 6901 so pointers stay unambiguous."""
    return str(key).replace("~", "~0").replace("/", "~1")


def tool_content_pointer(request: dict, tool_call_id: str) -> tuple[int, str]:
    """Locate the single string tool result for `tool_call_id`."""
    found = [
        (index, message.get("content"))
        for index, message in enumerate(request.get("messages", []))
        if message.get("role") == "tool" and message.get("tool_call_id") == tool_call_id
    ]
    if len(found) != 1 or not isinstance(found[0][1], str):
        raise ValueError("request must contain exactly one string result for the selected tool call")
    return found[0]


def issuing_assistant(messages: list, tool_index: int) -> dict:
    """Return the assistant message whose tool_calls produced `messages[tool_index]`.

    The immediately preceding message is NOT reliably that assistant: with
    parallel tool calls the predecessor is a sibling tool result.  Walk back to
    the nearest assistant instead, and fail fast rather than let a caller read
    tool_calls off a user or tool message and silently act on an empty list.
    """
    for index in range(tool_index - 1, -1, -1):
        message = messages[index]
        if message.get("role") != "assistant":
            continue
        if not message.get("tool_calls"):
            raise ValueError(
                f"assistant message at index {index} preceding tool result {tool_index} has no tool_calls"
            )
        return message
    raise ValueError(f"no preceding assistant message issues the tool result at index {tool_index}")


def diff_pointers(left: object, right: object, path: str = "") -> list[str]:
    """RFC 6901 pointers to every position where two request bodies differ."""
    if type(left) is not type(right):
        return [path or "/"]
    if isinstance(left, dict):
        result: list[str] = []
        for key in sorted(set(left) | set(right)):
            child = f"{path}/{_escape_pointer_token(key)}"
            result.extend([child] if key not in left or key not in right
                          else diff_pointers(left[key], right[key], child))
        return result
    if isinstance(left, list):
        if len(left) != len(right):
            return [path or "/"]
        return [item for index, pair in enumerate(zip(left, right))
                for item in diff_pointers(*pair, f"{path}/{index}")]
    return [] if left == right else [path or "/"]
