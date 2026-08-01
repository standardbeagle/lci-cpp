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


def digest(value: object) -> str:
    """Content digest of a value or an already-canonical string.

    Captured provider content can contain lone surrogates, which are legal in a
    Python `str` but not encodable as strict UTF-8.  `surrogatepass` digests
    them deterministically instead of aborting the replay on a data-dependent
    input; digests of well-formed text are unaffected.
    """
    text = value if isinstance(value, str) else canonical(value)
    return "sha256:" + hashlib.sha256(text.encode("utf-8", errors="surrogatepass")).hexdigest()


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
