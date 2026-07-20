#!/usr/bin/env python3
"""Score blinded evaluator judgments against labels withheld from judge prompts."""
from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path


def score(cases: dict, judgment_docs: list[dict]) -> dict:
    gold = {item["case_id"]: item for item in cases["cases"]}
    systems = {}
    for document in judgment_docs:
        system = document["judge_id"]
        records = {item["cell_key"]: item for item in document.get("records", [])}
        rows = []
        for case_id, case in gold.items():
            for repetition in (1, 2):
                judgment = records.get(f"{case_id}--r{repetition}", {}).get("judgment")
                predicted = judgment.get("verdict") if judgment else "unresolved"
                exact = predicted == case["gold_verdict"]
                rows.append({"case_id": case_id, "repetition": repetition,
                             "stratum": case["stratum"], "defect": case["defect"],
                             "gold": case["gold_verdict"], "predicted": predicted, "exact": exact})
        negatives = [row for row in rows if row["gold"] == "incorrect"]
        positives = [row for row in rows if row["gold"] == "correct"]
        by_stratum = defaultdict(lambda: {"n": 0, "exact": 0})
        for row in rows:
            by_stratum[row["stratum"]]["n"] += 1
            by_stratum[row["stratum"]]["exact"] += int(row["exact"])
        repeat_disagreements = sum(
            rows[index]["predicted"] != rows[index + 1]["predicted"]
            for index in range(0, len(rows), 2))
        systems[system] = {"n": len(rows), "exact": sum(row["exact"] for row in rows),
                           "accuracy": sum(row["exact"] for row in rows) / len(rows),
                           "false_accepts": sum(row["predicted"] == "correct" for row in negatives),
                           "false_rejects": sum(row["predicted"] == "incorrect" for row in positives),
                           "unresolved": sum(row["predicted"] in {"ambiguous", "unresolved"} for row in rows),
                           "repeat_disagreements": repeat_disagreements,
                           "by_stratum": dict(by_stratum), "rows": rows}
    gates = {system: {"accuracy_at_least_0_875": result["accuracy"] >= 0.875,
                      "false_accepts_at_most_2": result["false_accepts"] <= 2,
                      "repeat_disagreements_at_most_1": result["repeat_disagreements"] <= 1,
                      "all_cases_judged": result["unresolved"] == 0}
             for system, result in systems.items()}
    return {"schema": "lci.evaluator-meta-analysis.v1", "systems": systems, "gates": gates,
            "claim_boundary": "Measures verdict discrimination on 16 frozen paired continuation fixtures; not general evaluator accuracy."}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", type=Path, required=True)
    parser.add_argument("--judgments", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    result = score(json.loads(args.cases.read_text()),
                   [json.loads(path.read_text()) for path in args.judgments])
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
