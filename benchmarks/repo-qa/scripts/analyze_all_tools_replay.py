#!/usr/bin/env python3
"""Typed-claim analysis for the all-LCI-tools replay pilot."""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tool_claim_oracle import evaluate, load_bank


def analyze(cells: list[dict], matrix: dict, bank: dict) -> dict:
    cases = {f"{item['tool']}--{item['scenario']}": item for item in matrix["cases"]}
    schemas = {item["task_id"]: item for item in bank["schemas"]}
    scores, queue = [], []
    for cell in cells:
        if cell.get("status") != "answered":
            scores.append({"cell_key": cell.get("cell_key"), "status": "unscored_infrastructure"})
            continue
        case, schema = cases[cell["task_id"]], schemas[cell["task_id"]]
        result = evaluate(cell["final_answer"], schema)
        score = {"cell_key": cell["cell_key"], "task_id": cell["task_id"], "tool": case["tool"],
                 "scenario": case["scenario"], "model": cell["model"], "arm": cell["arm"],
                 "repetition": cell["repetition"], "answer": cell["final_answer"], **result}
        scores.append(score)
        if result["status"] == "needs_adjudication":
            queue.append({"cell_key": cell["cell_key"], "task_id": cell["task_id"],
                          "origin_model": cell["model"], "question": schema["question"],
                          "answer": cell["final_answer"],
                          "truth": {"source_tool_output": case["output"],
                                    "source_output_digest": schema["source_output_digest"],
                                    "required_claims": schema["required_claims"],
                                    "forbidden_patterns": schema.get("forbidden_patterns", [])},
                          "deterministic_result": result})
    grouped = defaultdict(lambda: {"n": 0, "accepted": 0, "needs_adjudication": 0})
    for score in scores:
        if "arm" in score:
            key = "|".join((score["tool"], score["scenario"], score["model"], score["arm"]))
            grouped[key]["n"] += 1
            grouped[key][score["status"]] += 1
    return {"schema": "lci.api-replay.all-tools-analysis.v2", "planned_cells": 448,
            "observed_cells": len(cells),
            "analysis_status": "pending_adjudication" if queue else "complete",
            "selection_allowed": False,
            "selection_reason": "historical transport pilot; repaired oracle is exploratory until preregistered rerun",
            "adjudication_queue": queue, "scores": scores, "by_stratum": dict(grouped)}


def load_cells(path: Path) -> list[dict]:
    attempts = path / "attempts"
    source = attempts if attempts.is_dir() else path
    return [json.loads(item.read_text()) for item in sorted(source.glob("*.json"))]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cells", type=Path, required=True)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--claims", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    matrix = json.loads(args.matrix.read_text())
    bank = load_bank(args.claims, args.matrix)
    report = analyze(load_cells(args.cells), matrix, bank)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
