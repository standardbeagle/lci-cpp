#!/usr/bin/env python3
"""Shared core for the two LLM adjudicators.

`llm_oracle_adjudicator.py` and `llm_tool_result_adjudicator.py` each carried
their own header-profile loader, provider request builder, code-fence stripper,
and checkpoint writer.  The copies had drifted (IndexError vs silent fallback on
a bare model id; missing temp-file cleanup in one checkpoint writer; fence
stripping that dropped the final JSON line when the closing fence was absent),
so the two judges could fail differently on identical provider output.  One
implementation here keeps them byte-for-byte aligned.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay_common


def model_suffix(model: str) -> str:
    """Provider-local model id from a full `provider/model` id.

    Fails with a named error instead of a bare IndexError so a bare alias is
    diagnosed at the call site, not inside a request-body expression.
    """
    provider, separator, suffix = model.partition("/")
    if not provider or not separator or not suffix:
        raise ValueError(f"model id must be a full 'provider/model' id, got {model!r}")
    return suffix


def request_body(model: str, prompt: str, *, system: str) -> dict:
    return {"model": model_suffix(model), "temperature": 0, "stream": False,
            "messages": [{"role": "system", "content": system},
                         {"role": "user", "content": prompt}]}


def strip_code_fence(text: str) -> str:
    """Return the payload of a leading markdown code fence, or the text itself.

    Handles a missing closing fence (keep everything after the opener) and
    trailing prose after the closing fence (drop it) without ever discarding
    the final JSON line, unlike a naive `splitlines()[1:-1]`.
    """
    stripped = text.strip()
    if not stripped.startswith("```"):
        return stripped
    lines = stripped.splitlines()[1:]
    for index, line in enumerate(lines):
        if line.strip() == "```":
            return "\n".join(lines[:index]).strip()
    return "\n".join(lines).strip()


def load_header_profile(path: Path) -> dict:
    """Header profiles are committed with a `headers` key; anything else is a bug."""
    document = json.loads(path.read_text())
    headers = document.get("headers")
    if not isinstance(headers, dict):
        raise SystemExit(
            f"{path}: header profile must carry a 'headers' object, found keys {sorted(document)}"
        )
    return headers


def write_checkpoint(path: Path, document: dict) -> None:
    """Atomically replace the checkpoint; failed writes leave no temp debris."""
    replay_common.write_atomic(path, json.dumps(document, indent=2, sort_keys=True) + "\n")
