#!/usr/bin/env python3
"""Build blinded exploratory design-interview prompts from replay fixtures."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from replay_common import issuing_assistant  # noqa: E402  (path bootstrap must precede the import)


def tool_exchange(fixture: dict) -> tuple[dict, dict, dict]:
    messages = fixture["request"]["messages"]
    tool_id = fixture["tool_call_id"]
    tool_index = next(
        index for index, message in enumerate(messages)
        if message.get("role") == "tool" and message.get("tool_call_id") == tool_id
    )
    assistant = issuing_assistant(messages, tool_index)
    user = next(message for message in reversed(messages[:tool_index]) if message.get("role") == "user")
    return user, assistant, messages[tool_index]


def build_prompt(fixture: dict) -> str:
    user, assistant, tool = tool_exchange(fixture)
    context = {
        "user_message": user["content"],
        "assistant_tool_call": assistant.get("tool_calls", []),
        "production_tool_result": tool["content"],
    }
    return """You are advising on the wire representation of a coding-agent tool result.
This is exploratory design research, not a performance evaluation. Review the genuine
OpenCode exchange below. Do not infer or supply the final answer to the user's task.

Propose at most three alternative representations, and explicitly include "keep the
production representation" if that is preferable. Every alternative must:
- preserve exactly the facts in production_tool_result, adding no status, conclusion,
  next action, explanation, or outside fact;
- be mechanically and unambiguously invertible to the original JSON value;
- generalize to empty, partial, error, and multi-result outputs;
- identify an exact deterministic forward and inverse transformation.

For each proposal report: a short name, deterministic transformation, inverse,
worked representation of this exact result, likely benefit, and likely failure mode.
Finish with a ranked recommendation and the smallest fair experiment that could
falsify it. Do not cite model intuition as evidence.

CAPTURED EXCHANGE (final response and benchmark oracle intentionally omitted):
""" + json.dumps(context, sort_keys=True, indent=2, ensure_ascii=False)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    prompt = build_prompt(json.loads(args.fixture.read_text()))
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(prompt + "\n")
    else:
        print(prompt)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
