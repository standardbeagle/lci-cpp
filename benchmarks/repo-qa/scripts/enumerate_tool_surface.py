#!/usr/bin/env python3
"""Capture the live LCI MCP surface and its independent-oracle task bank."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
from pathlib import Path


CASES = {
    "browse_file": {
        "question": "What exported methods are declared in pocketbase.go?",
        "oracle_command": "rg -n '^func \\(pb \\*PocketBase\\) [A-Z]' pocketbase.go",
        "answer": ["Execute", "Start"],
    },
    "code_insight": {
        "question": "How many Go source files are in the repository?",
        "oracle_command": "find . -type f -name '*.go' -print | LC_ALL=C sort | wc -l",
        "answer": ["447"],
    },
    "context": {
        "question": "What definition and documentation describe PocketBase.Start?",
        "oracle_command": "sed -n '160,181p' pocketbase.go",
        "answer": ["Start starts the application", "func (pb *PocketBase) Start() error"],
    },
    "debug_info": {
        "question": "Which file declares PocketBase.Start?",
        "oracle_command": "rg -l '^func \\(pb \\*PocketBase\\) Start' --glob '*.go'",
        "answer": ["pocketbase.go"],
    },
    "find_files": {
        "question": "Which Go files have record_upsert in their path?",
        "oracle_command": "find . -type f -name '*record_upsert*.go' -printf '%P\\n' | LC_ALL=C sort",
        "answer": ["forms/record_upsert.go", "forms/record_upsert_test.go"],
    },
    "get_context": {
        "question": "What is the definition and documentation of PocketBase.Start?",
        "oracle_command": "gopls definition pocketbase.go:166:24",
        "answer": ["func (pb *PocketBase) Start() error", "Start starts the application"],
    },
    "git_analysis": {
        "question": "What commit is checked out for this corpus?",
        "oracle_command": "git rev-parse HEAD",
        "answer": ["d438c6a96a0252ff9df62c1cfe193480ed9ff15d"],
    },
    "index_stats": {
        "question": "Is the index ready to answer queries?",
        "oracle_command": "find . -type f -name '*.go' -print -quit | grep -q . && printf ready",
        "answer": ["ready"],
        "control": True,
    },
    "info": {
        "question": "What is the purpose of the search operation?",
        "oracle_command": "printf '%s\\n' 'searches indexed source text'",
        "answer": ["searches indexed source text"],
        "control": True,
    },
    "inspect_symbol": {
        "question": "Where is PocketBase.Start defined?",
        "oracle_command": "gopls definition pocketbase.go:166:24",
        "answer": ["pocketbase.go:166", "func (pb *PocketBase) Start() error"],
    },
    "list_symbols": {
        "question": "Which exported PocketBase methods are declared in pocketbase.go?",
        "oracle_command": "rg -o '^func \\(pb \\*PocketBase\\) [A-Z][A-Za-z0-9_]+' pocketbase.go | awk '{print $4}' | LC_ALL=C sort",
        "answer": ["Execute", "Start"],
    },
    "search": {
        "question": "Where is PocketBase.Start called in non-test Go source?",
        "oracle_command": "gopls references -d examples/base/main.go:119:16",
        "answer": ["examples/base/main.go:119", "pocketbase.go:166"],
    },
    "semantic_annotations": {
        "question": "Which Go source lines contain an @lci semantic annotation?",
        "oracle_command": "rg -n '@lci:' --glob '*.go' .",
        "oracle_allowed_exit_codes": [0, 1],
        "answer": [],
        "control": True,
    },
    "side_effects": {
        "question": "Which method does PocketBase.Start directly invoke to execute the application?",
        "oracle_command": "sed -n '166,175p' pocketbase.go | rg -o 'pb\\.[A-Za-z0-9_]+\\(' | sed -E 's/^pb\\.//; s/\\($//' | LC_ALL=C sort -u",
        "answer": ["Execute"],
    },
}


class McpSession:
    def __init__(self, executable: Path, corpus: Path):
        self.proc = subprocess.Popen(
            [str(executable), "mcp"], cwd=corpus, text=True,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        self.next_id = 1
        self.rpc("initialize", {
            "protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "surface-enumerator", "version": "1"},
        })
        self.notify("notifications/initialized")

    def send(self, value: dict) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(json.dumps(value, separators=(",", ":")) + "\n")
        self.proc.stdin.flush()

    def notify(self, method: str) -> None:
        self.send({"jsonrpc": "2.0", "method": method})

    def rpc(self, method: str, params: dict | None = None) -> dict:
        request_id = self.next_id
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            request["params"] = params
        self.send(request)
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            response = json.loads(line)
            if response.get("id") == request_id:
                if "error" in response:
                    raise RuntimeError(response["error"])
                return response["result"]
        raise RuntimeError(f"LCI MCP exited during {method}")

    def close(self) -> None:
        if self.proc.stdin:
            self.proc.stdin.close()
        self.proc.wait(timeout=15)


def build_manifest(tools: list[dict], corpus: Path) -> dict:
    by_name = {tool["name"]: tool for tool in tools}
    if set(by_name) != set(CASES):
        missing = sorted(set(by_name) - set(CASES))
        stale = sorted(set(CASES) - set(by_name))
        raise ValueError(f"surface mismatch: missing_cases={missing}, stale_cases={stale}")
    corpus_commit = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=corpus, check=True,
        text=True, capture_output=True,
    ).stdout.strip()
    gopls_version = subprocess.run(
        ["gopls", "version"], check=True, text=True, capture_output=True,
    ).stdout.strip().splitlines()[-1]
    entries = []
    for name in sorted(by_name):
        live = by_name[name]
        case = CASES[name]
        oracle_output = run_oracle(
            case["oracle_command"], corpus,
            set(case.get("oracle_allowed_exit_codes", [0])),
        )
        missing_answers = [answer for answer in case["answer"] if answer not in oracle_output]
        if missing_answers:
            raise ValueError(
                f"oracle for {name} does not support answers: {missing_answers}"
            )
        entries.append({
            "name": name,
            "description": live.get("description", ""),
            "input_schema": live.get("inputSchema"),
            "output_schema": live.get("outputSchema"),
            **case,
            "oracle_output": oracle_output,
        })
    return {
        "schema": "lci.comprehension.tool-surface.v1",
        "source": "live JSON-RPC tools/list from `lci mcp`",
        "corpus": "pocketbase",
        "corpus_commit": corpus_commit,
        "gopls_version": gopls_version,
        "tool_count": len(entries),
        "tools": entries,
    }


def run_oracle(
    command: str, corpus: Path, allowed_exit_codes: set[int] | None = None,
) -> str:
    """Execute one curated independent oracle, failing closed on any error."""
    env = os.environ.copy()
    env["GOCACHE"] = "/tmp/lci-comprehension-gocache"
    result = subprocess.run(
        ["bash", "-lc", command], cwd=corpus.resolve(), env=env,
        text=True, capture_output=True,
    )
    allowed = {0} if allowed_exit_codes is None else allowed_exit_codes
    if result.returncode not in allowed:
        raise RuntimeError(
            f"oracle failed ({result.returncode}): {command}\n{result.stderr.strip()}"
        )
    root = str(corpus.resolve())
    normalized = result.stdout.replace(root + "/", "").replace(root, ".")
    return normalized.rstrip("\n")


def canonical_json(value: dict) -> str:
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lci-bin", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    session = McpSession(args.lci_bin.resolve(), args.corpus.resolve())
    try:
        manifest = build_manifest(session.rpc("tools/list")["tools"], args.corpus)
    finally:
        session.close()
    encoded = canonical_json(manifest)
    if args.check:
        if not args.output.exists() or args.output.read_text() != encoded:
            raise SystemExit("committed tool-surface manifest is stale")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
