#!/usr/bin/env python3
"""Compare mechanically conformant codecs without making model-quality claims."""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path

import conformance


def source_values(bank: dict, fixture: dict | None) -> list[dict]:
    values = [{"id": case["id"], "value": case["value"]} for case in bank["cases"]]
    if fixture is not None:
        tool_id = fixture["tool_call_id"]
        messages = fixture["request"]["messages"]
        matches = [m for m in messages if m.get("role") == "tool" and m.get("tool_call_id") == tool_id]
        if len(matches) != 1:
            raise ValueError("fixture must contain exactly one selected tool result")
        values.append({"id": "live:" + fixture["task_id"], "value": json.loads(matches[0]["content"])})
    return values


def measure(module, values: list[dict]) -> dict:
    rows = []
    for item in values:
        source = conformance.canonical(item["value"])
        # Match conformance.evaluate_case: never hand a candidate the shared
        # value, or one mutating encode corrupts every later measurement row.
        rendered = module.encode(copy.deepcopy(item["value"]))
        rows.append({
            "id": item["id"],
            "source_utf8_bytes": len(source.encode("utf-8")),
            "rendered_utf8_bytes": len(rendered.encode("utf-8", errors="surrogatepass")),
            "rendered_characters": len(rendered),
            "rendered_lines": rendered.count("\n") + 1,
        })
    source_bytes = sum(row["source_utf8_bytes"] for row in rows)
    rendered_bytes = sum(row["rendered_utf8_bytes"] for row in rows)
    return {
        "candidate": getattr(module, "NAME", module.__name__),
        "source_utf8_bytes": source_bytes,
        "rendered_utf8_bytes": rendered_bytes,
        "byte_ratio": rendered_bytes / source_bytes,
        "cases": rows,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("candidates", nargs="+", type=Path)
    parser.add_argument("--bank", type=Path, default=conformance.DEFAULT_BANK)
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    bank = conformance.load_bank(args.bank)
    fixture = json.loads(args.fixture.read_text()) if args.fixture else None
    values = source_values(bank, fixture)
    candidates = []
    for path in args.candidates:
        module = conformance.load_candidate(path)
        report = conformance.evaluate(module, bank)
        if not report["summary"]["conformant"]:
            raise ValueError(f"nonconformant candidate: {path}")
        candidates.append(measure(module, values))
    result = {"schema": "lci.api-replay.candidate-comparison.v1", "candidates": candidates}
    output = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.out:
        args.out.write_text(output)
    else:
        print(output, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
