#!/usr/bin/env python3
"""Capture genuine success and negative-path outputs for every live LCI MCP tool."""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("surface", HERE / "enumerate_tool_surface.py")
surface = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(surface)

# Negative paths are valid tool requests selected before observing their output.
# They exercise missing entities, empty result sets, or invalid domain values rather
# than merely corrupting the JSON-RPC envelope.
NEGATIVE_ARGUMENTS = {
    "browse_file": {"file": "definitely/not/a/real/file.go", "max": 10},
    "code_insight": {"mode": "detailed", "target": "definitely/not/a/real/directory"},
    "context": {"operation": "load", "from_file": "definitely-not-a-real-context.json"},
    "debug_info": {"mode": "files", "file_path": "definitely/not/a/real/file.go"},
    "find_files": {"pattern": "definitely_no_such_filename_7f31c9", "max": 10},
    "get_context": {"name": "DefinitelyNoSuchSymbol7f31c9", "mode": "compact"},
    "git_analysis": {"scope": "range", "base_ref": "definitely-no-such-ref", "target_ref": "HEAD"},
    "index_stats": {"mode": "definitely-not-a-mode"},
    "info": {"tool": "definitely-not-a-tool"},
    "inspect_symbol": {"name": "DefinitelyNoSuchSymbol7f31c9", "include": "signature,doc"},
    "list_symbols": {"kind": "method", "file": "definitely/not/a/real/file.go", "max": 10},
    "search": {"pattern": "DefinitelyNoSuchSymbol7f31c9", "max": 10},
    "semantic_annotations": {"label": "definitely-no-such-label", "max_results": 10},
    "side_effects": {"mode": "symbol", "symbol_name": "DefinitelyNoSuchSymbol7f31c9", "max_results": 10},
}


def text_content(result: dict) -> str:
    parts = result.get("content")
    if not isinstance(parts, list):
        raise ValueError("tools/call result has no content array")
    texts = [part["text"] for part in parts if isinstance(part, dict)
             and part.get("type") == "text" and isinstance(part.get("text"), str)]
    if not texts:
        raise ValueError("tools/call result has no text content")
    return "\n".join(texts)


def capture(session, successes: dict) -> dict:
    live = {tool["name"] for tool in session.rpc("tools/list")["tools"]}
    if live != set(successes) or live != set(NEGATIVE_ARGUMENTS):
        raise ValueError(f"tool surface mismatch: live={sorted(live)}")
    cases = []
    for name in sorted(live):
        for scenario, arguments in (("success", successes[name]),
                                    ("negative", NEGATIVE_ARGUMENTS[name])):
            result = session.rpc("tools/call", {"name": name, "arguments": arguments})
            cases.append({"tool": name, "scenario": scenario, "arguments": arguments,
                          "is_error": bool(result.get("isError", False)),
                          "output": text_content(result)})
    return {"schema": "lci.tool-output-matrix.v1", "selection": "all live tools",
            "scenario_policy": "one frozen success and one preselected domain-negative request per tool",
            "tool_count": len(live), "case_count": len(cases), "cases": cases}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lci-bin", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--success-captures", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    source = json.loads(args.success_captures.read_text())
    successes = {item["name"]: item["arguments"] for item in source["captures"]}
    session = surface.McpSession(args.lci_bin.resolve(), args.corpus.resolve())
    try:
        document = capture(session, successes)
    finally:
        session.close()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
