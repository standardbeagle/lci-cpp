#!/usr/bin/env python3
"""Heuristic-first analysis for the all-LCI-tools replay pilot."""
from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path


NEGATIVE_WORDS = re.compile(r"\b(?:no|none|zero|empty|not found|did not|failed?|error)\b", re.I)
ERROR_WORDS = re.compile(r"\b(?:failed?|error|not found|invalid|unable)\b", re.I)


def negative_class(case: dict) -> str:
    if case["is_error"]:
        return "error"
    text = case["output"]
    if re.search(r'\b(?:0 files|0 matches|no symbol|no indexed file|no @lci)', text, re.I):
        return "empty_or_not_found"
    try:
        value = json.loads(text)
    except json.JSONDecodeError:
        return "corrective_or_fallback"
    if isinstance(value, dict) and any(value.get(key) in ([], 0) for key in
                                       ("results", "symbols", "contexts", "annotations", "count", "showing", "total")):
        return "empty_or_not_found"
    return "corrective_or_fallback"


def score_answer(answer: str, case: dict, surface: dict) -> dict:
    folded = " ".join(answer.casefold().split())
    if case["scenario"] == "success":
        atoms = [str(item).casefold() for item in surface["answer"]]
        if atoms:
            passed = all(atom in folded for atom in atoms)
            reason = "all_required_atoms" if passed else "missing_required_atoms"
        else:
            passed = bool(NEGATIVE_WORDS.search(answer))
            reason = "correct_empty_result" if passed else "empty_result_not_acknowledged"
        expected = {"kind": "success", "required_atoms": atoms}
    else:
        kind = negative_class(case)
        passed = bool(ERROR_WORDS.search(answer)) if kind == "error" else \
                 bool(NEGATIVE_WORDS.search(answer)) if kind == "empty_or_not_found" else \
                 len(answer.strip()) > 0
        reason = f"recognized_{kind}" if passed else f"did_not_recognize_{kind}"
        expected = {"kind": kind, "source_tool_output": case["output"]}
    return {"heuristic_pass": passed, "heuristic_reason": reason, "expected": expected,
            "status": "accepted" if passed else "needs_adjudication"}


def analyze(cells: list[dict], matrix: dict, surface_doc: dict) -> dict:
    cases = {f"{item['tool']}--{item['scenario']}": item for item in matrix["cases"]}
    surfaces = {item["name"]: item for item in surface_doc["tools"]}
    scores, queue = [], []
    for cell in cells:
        if cell.get("status") != "answered":
            scores.append({"cell_key": cell.get("cell_key"), "status": "unscored_infrastructure"})
            continue
        case = cases[cell["task_id"]]
        result = score_answer(cell["final_answer"], case, surfaces[case["tool"]])
        score = {"cell_key": cell["cell_key"], "task_id": cell["task_id"], "tool": case["tool"],
                 "scenario": case["scenario"], "model": cell["model"], "arm": cell["arm"],
                 "repetition": cell["repetition"], "answer": cell["final_answer"], **result}
        scores.append(score)
        if result["status"] == "needs_adjudication":
            queue.append({"cell_key": cell["cell_key"], "task_id": cell["task_id"],
                          "question": surfaces[case["tool"]]["question"] if case["scenario"] == "success" else
                                      f"Interpret the negative {case['tool']} result.",
                          "answer": cell["final_answer"], "expected": result["expected"]})
    grouped = defaultdict(lambda: {"n": 0, "heuristic_pass": 0})
    for score in scores:
        if "arm" in score:
            key = "|".join((score["tool"], score["scenario"], score["model"], score["arm"]))
            grouped[key]["n"] += 1
            grouped[key]["heuristic_pass"] += int(score["heuristic_pass"])
    return {"schema": "lci.api-replay.all-tools-analysis.v1", "planned_cells": 448,
            "observed_cells": len(cells), "analysis_status": "pending_adjudication" if queue else "complete",
            "selection_allowed": False, "selection_reason": "two-repetition transport/oracle pilot",
            "adjudication_queue": queue, "scores": scores, "by_stratum": dict(grouped)}


def load_cells(path: Path) -> list[dict]:
    return [json.loads(item.read_text()) for item in sorted(path.glob("*.json"))]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cells", type=Path, required=True)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--surface", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    report = analyze(load_cells(args.cells), json.loads(args.matrix.read_text()), json.loads(args.surface.read_text()))
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
