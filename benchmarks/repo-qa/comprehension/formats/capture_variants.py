#!/usr/bin/env python3
"""Capture normalized live MCP outputs and update current format variants."""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import replay_common


ARGS = {
    "browse_file": {"file": "pocketbase.go", "exported": True, "max": 20},
    "code_insight": {"analysis": "overview", "max_results": 10},
    "context": {"operation": "save", "refs": [{"f": "pocketbase.go", "l": {"start": 160, "end": 181}}], "to_string": True},
    "debug_info": {"mode": "files", "file_path": "pocketbase.go", "max_results": 10},
    "find_files": {"pattern": "record_upsert", "filter": "go", "max": 10},
    "get_context": {"name": "Start", "mode": "compact"},
    "git_analysis": {"scope": "commit", "base_ref": "HEAD", "max_findings": 5},
    "index_stats": {"mode": "summary"},
    "info": {"tool": "search"},
    "inspect_symbol": {"name": "Start", "file": "pocketbase.go", "include": "signature,doc,refs"},
    "list_symbols": {"kind": "method", "file": "pocketbase.go", "exported": True, "max": 20},
    "search": {"pattern": "app.Start()", "flags": "nt", "max": 20},
    "semantic_annotations": {"label": "critical", "max_results": 10},
    "side_effects": {"mode": "symbol", "symbol_name": "Start", "include_reasons": True, "max_results": 10},
}

VOLATILE_KEYS = {"analyzed_at", "analysis_time_ms", "index_time_ms", "timestamp", "timestamp_ms"}


canonical = replay_common.pretty  # byte-identical to the previous local copy


def normalize_text(text: str, corpus: Path) -> str:
    text = text.replace(str(corpus.resolve()), "<CORPUS_ROOT>")
    text = re.sub(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?(?:Z|[+-]\d\d:\d\d)", "<TIMESTAMP>", text)
    try:
        value = json.loads(text)
    except json.JSONDecodeError:
        return text.strip()

    def clean(item):
        if isinstance(item, dict):
            return {
                key: clean(val) for key, val in item.items()
                if key not in VOLATILE_KEYS
                and not key.endswith("_time")
                and not key.endswith("_time_ms")
            }
        if isinstance(item, list):
            return [clean(value) for value in item]
        if isinstance(item, str):
            return item.replace(str(corpus.resolve()), "<CORPUS_ROOT>")
        return item

    return json.dumps(clean(value), sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def load_session_class(repo_root: Path):
    path = repo_root / "benchmarks/repo-qa/scripts/tooleval.py"
    spec = importlib.util.spec_from_file_location("tooleval_for_capture", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module.McpSession


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lci-bin", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--variants", type=Path, required=True)
    parser.add_argument("--captures", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[4]
    session_cls = load_session_class(root)
    session = session_cls(str(args.lci_bin.resolve()), str(args.corpus.resolve()))
    records = []
    try:
        for name in sorted(ARGS):
            payload, _ = session.call_tool(name, ARGS[name])
            if "text" not in payload:
                raise RuntimeError(f"live capture failed for {name}: {payload}")
            normalized = normalize_text(payload["text"], args.corpus)
            records.append({"name": name, "arguments": ARGS[name], "normalized_output": normalized})
    finally:
        session.close()

    captures = {"schema": "lci.comprehension.live-captures.v1", "captures": records}
    variants = json.loads(args.variants.read_text())
    captured = {record["name"]: record["normalized_output"] for record in records}
    for tool in variants["tools"]:
        current = next(v for v in tool["variants"] if v["id"] == "current")
        current["text"] = captured[tool["name"]]
    args.captures.write_text(canonical(captures))
    args.variants.write_text(canonical(variants))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
