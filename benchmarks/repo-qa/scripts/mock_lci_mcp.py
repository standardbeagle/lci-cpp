#!/usr/bin/env python3
"""Hermetic stdio MCP that mimics LCI's tool NAMES with injectable descriptions.

Why a mock at all: the tool-selection bench measures which tool an agent PICKS
for a stated goal. A real LCI server folds three other variables into that
measurement -- output comprehension, real indexing latency, and real LCI
defects. The mock removes all three: every handler returns a deterministic stub
that merely records the call, so a wrong answer can only come from selection.

Why the names come from the manifest: the tool list is read from the committed
live-surface manifest (comprehension/surface/tool-surface.json, itself probed
from the real binary by scripts/enumerate_tool_surface.py). Hand-listing them
here would let the mock drift from the surface, and a drifted mock silently
measures selection over a tool set that does not exist.

Descriptions are injectable (E2.2 swaps wording variants and measures the shift
in the confusion matrix): a JSON file named by --descriptions or by the
LCI_MOCK_DESCRIPTIONS environment variable, defaulting to the committed
baseline. Nothing else about the server changes between variants.

Protocol: newline-delimited JSON-RPC on stdin/stdout with a negotiated
protocolVersion -- the framing the real LCI `mcp` subcommand speaks. The argv
tail is ignored so the process can be spawned as `<script> mcp`, the same shape
tooleval.McpSession uses for the real binary.

This module also hosts the two task-set gates (neighbor_coverage,
find_tool_name_leaks) because the E2.1 file scope keeps the toolcalling
mechanism in one script; both are pure functions over already-loaded tasks.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

REPO_QA = Path(__file__).resolve().parents[1]
MANIFEST = REPO_QA / "comprehension/surface/tool-surface.json"
BASELINE_DESCRIPTIONS = REPO_QA / "toolcalling/mock_mcp/descriptions-baseline.json"
PROTOCOL_VERSION = "2025-06-18"


# ---------------------------------------------------------------- tool surface

def load_tool_names(manifest: Path = MANIFEST) -> list[str]:
    return sorted(t["name"] for t in json.loads(manifest.read_text())["tools"])


def load_input_schemas(manifest: Path = MANIFEST) -> dict[str, dict]:
    """Per-tool input schemas as the live binary serves them.

    Serving a stub schema would hide every parameter from the agent, so a
    selection arm could only guess argument names and no arm could carry the
    "input-schema hints" the experiment varies. Like the names, these come from
    the probed manifest so the mock cannot drift from the real surface.
    """
    return {t["name"]: t["input_schema"]
            for t in json.loads(manifest.read_text())["tools"]}


def load_descriptions(path: Path | None = None) -> dict[str, str]:
    path = path or BASELINE_DESCRIPTIONS
    payload = json.loads(Path(path).read_text())
    return dict(payload["descriptions"])


# ------------------------------------------------------------ task-set gates

# camelCase / PascalCase / ALLCAPS / digit-run aware splitter; separators
# (/ \ . - _ space) are simply unmatched and act as token boundaries. Mirrors
# scripts/lint_exploration_leaks.py so the two linters agree on "same word".
_WORD = re.compile(r"[A-Z]+(?=[A-Z][a-z])|[A-Z]?[a-z]+|[A-Z]+|[0-9]+")

# Segments that are ordinary domain vocabulary rather than a tool fingerprint.
# A prompt MUST be able to say "which file holds this symbol" without tripping
# the gate. This allowlist only SUPPRESSES matches; forbidden material is
# always derived from the live tool names, never written down here
# (.claude/rules/bench-harness-oracle-independence.md 1).
GENERIC_SEGMENTS = {
    "analysis", "annotations", "code", "context", "debug", "effects", "file",
    "files", "find", "get", "git", "index", "info", "insight", "list",
    "semantic", "side", "stats", "symbol", "symbols",
}


def tokens(text: str) -> list[str]:
    return [m.group(0).lower() for m in _WORD.finditer(text)]


def _squash(text: str) -> str:
    return "".join(tokens(text))


def forbidden_forms(tool_names: list[str]) -> dict[str, list[list[str]]]:
    """Derive, per tool, every token sequence that would give that tool away.

    Two granularities (rule 6): the full name in any separator/case spelling,
    and any single segment that belongs to exactly one tool and is not generic
    domain vocabulary -- "search", "browse", "inspect" identify a tool on their
    own as surely as the full name does.
    """
    counts: dict[str, int] = {}
    for name in tool_names:
        for segment in set(tokens(name)):
            counts[segment] = counts.get(segment, 0) + 1
    forms: dict[str, list[list[str]]] = {}
    for name in tool_names:
        seq = tokens(name)
        variants = [seq]
        for segment in seq:
            if counts[segment] == 1 and segment not in GENERIC_SEGMENTS:
                if [segment] not in variants:
                    variants.append([segment])
        forms[name] = variants
    return forms


def _redact(text: str) -> str:
    if len(text) <= 2:
        return f"<{len(text)} chars>"
    return f"{text[0]}...{text[-1]} ({len(text)} chars)"


def find_tool_name_leaks(tasks: list[dict], tool_names: list[str]) -> list[dict]:
    """Report every task whose agent-visible text names a tool.

    Only agent-visible fields are scanned; `rationale`, `correct_tool` and
    `confusable_neighbors` are author-side answer key and stay readable for
    scoring. Matches are reported REDACTED so a CI log never carries the key.
    """
    forms = forbidden_forms(tool_names)
    leaks: list[dict] = []
    for task in tasks:
        text = task.get("prompt", "")
        toks = tokens(text)
        squashed = _squash(text)
        for tool, variants in forms.items():
            for variant in variants:
                width = len(variant)
                windowed = any(toks[i:i + width] == variant
                               for i in range(len(toks) - width + 1))
                joined = "".join(variant)
                if windowed or (width > 1 and joined in squashed):
                    leaks.append({
                        "task_id": task.get("id"),
                        "tool": _redact(tool),
                        "matched": _redact(joined),
                    })
                    break
    return leaks


def neighbor_coverage(tasks: list[dict]) -> set[str]:
    """Tools that appear as a confusable neighbor of some OTHER tool's task.

    A tool absent here is a column of the confusion matrix nothing can ever
    populate: no prompt exists where a model could plausibly mis-pick it.
    """
    covered: set[str] = set()
    for task in tasks:
        for neighbor in task.get("confusable_neighbors", []):
            if neighbor != task.get("correct_tool"):
                covered.add(neighbor)
    return covered


# ------------------------------------------------------------------ mock server

class MockServer:
    def __init__(self, tool_names: list[str], descriptions: dict[str, str],
                 input_schemas: dict[str, dict], call_log: Path | None = None):
        self.tool_names = tool_names
        self.descriptions = descriptions
        self.input_schemas = input_schemas
        self.call_log = call_log

    def tools_list(self) -> dict:
        return {"tools": [{
            "name": name,
            "description": self.descriptions[name],
            "inputSchema": self.input_schemas[name],
        } for name in self.tool_names]}

    def call(self, name: str, arguments: dict) -> dict:
        if name not in self.descriptions:
            raise KeyError(name)
        rendered = json.dumps(arguments, sort_keys=True, ensure_ascii=False,
                              separators=(", ", ": "))
        text = f"OK: called {name} with {rendered}"
        if self.call_log is not None:
            with self.call_log.open("a", encoding="utf-8") as fh:
                fh.write(json.dumps({"tool": name, "arguments": arguments},
                                    sort_keys=True) + "\n")
        return {"content": [{"type": "text", "text": text}], "isError": False}

    def handle(self, msg: dict) -> dict | None:
        method = msg.get("method")
        rid = msg.get("id")
        if rid is None:  # notification
            return None
        if method == "initialize":
            result = {"protocolVersion": PROTOCOL_VERSION,
                      "capabilities": {"tools": {}},
                      "serverInfo": {"name": "mock-lci", "version": "1"}}
        elif method == "tools/list":
            result = self.tools_list()
        elif method == "tools/call":
            params = msg.get("params") or {}
            try:
                result = self.call(params.get("name", ""),
                                   params.get("arguments") or {})
            except KeyError as exc:
                return {"jsonrpc": "2.0", "id": rid,
                        "error": {"code": -32602,
                                  "message": f"unknown tool: {exc.args[0]}"}}
        else:
            return {"jsonrpc": "2.0", "id": rid,
                    "error": {"code": -32601, "message": f"unknown method: {method}"}}
        return {"jsonrpc": "2.0", "id": rid, "result": result}

    def serve(self, stdin, stdout) -> None:
        for line in stdin:
            line = line.strip()
            if not line:
                continue
            response = self.handle(json.loads(line))
            if response is not None:
                stdout.write(json.dumps(response) + "\n")
                stdout.flush()


def build_server(descriptions_path: Path | None = None,
                 call_log: Path | None = None) -> MockServer:
    names = load_tool_names()
    descriptions = load_descriptions(descriptions_path)
    missing = sorted(set(names) - set(descriptions))
    extra = sorted(set(descriptions) - set(names))
    if missing or extra:
        raise SystemExit(
            f"description set does not match the live surface: "
            f"missing={missing} unexpected={extra}")
    return MockServer(names, descriptions, load_input_schemas(), call_log)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--descriptions", type=Path, default=None,
                        help="description variant JSON (default: committed baseline)")
    parser.add_argument("--call-log", type=Path, default=None,
                        help="append one JSON line per tool call")
    # The argv tail lets the process be spawned as `<script> mcp`, matching the
    # real binary's invocation so the same stdio client drives both.
    parser.add_argument("mode", nargs="?", default="mcp")
    args = parser.parse_args(argv)

    descriptions = args.descriptions
    if descriptions is None and os.environ.get("LCI_MOCK_DESCRIPTIONS"):
        descriptions = Path(os.environ["LCI_MOCK_DESCRIPTIONS"])
    call_log = args.call_log
    if call_log is None and os.environ.get("LCI_MOCK_CALL_LOG"):
        call_log = Path(os.environ["LCI_MOCK_CALL_LOG"])

    build_server(descriptions, call_log).serve(sys.stdin, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
