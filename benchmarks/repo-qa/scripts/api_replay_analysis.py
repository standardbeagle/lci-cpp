#!/usr/bin/env python3
"""Frozen analysis for the exploratory four-format captured-request replay."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import statistics
from collections import defaultdict
from pathlib import Path


SCHEMA = "lci.api-replay.format-analysis.v1"
SCORE_SCHEMA = "lci.api-replay.format-score.v1"
ANSWERED = "answered"
BASELINE = "fmt_07"
LOCATION = re.compile(r"(?<![\w./-])([\w.@+-]+(?:/[\w.@+-]+)+:\d+)(?![\w/:-])")


class AnalysisError(ValueError):
    pass


def normalize(value: str) -> str:
    return " ".join(value.casefold().split())


def canonical_digest(value: object) -> str:
    encoded = json.dumps(value, ensure_ascii=True, allow_nan=False, sort_keys=True,
                         separators=(",", ":")).encode()
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def load_oracles(fixtures_dir: Path, task_id: str = "search-callsite-v2") -> dict[str, dict]:
    oracles = {}
    for path in sorted(fixtures_dir.glob("fixture-*-search.json")):
        fixture = json.loads(path.read_text())
        if fixture.get("task_id") != task_id:
            raise AnalysisError(f"fixture task must be {task_id}: {path}")
        model = fixture.get("recorded_model")
        expected = fixture.get("expected_answers")
        if not isinstance(model, str) or not isinstance(expected, list) or not expected:
            raise AnalysisError(f"invalid model oracle in {path}")
        normalized = [normalize(item) for item in expected]
        if not all(LOCATION.fullmatch(item) for item in normalized):
            raise AnalysisError(f"unsupported frozen oracle shape in {path}")
        if model in oracles:
            raise AnalysisError(f"duplicate fixture for model {model}")
        oracles[model] = {"task_id": task_id, "expected": normalized,
                          "kind": "source_locations_v1", "fixture": str(path),
                          "fixture_digest": canonical_digest(fixture)}
    return oracles


def extract(answer: str) -> list[str]:
    return sorted({normalize(match) for match in LOCATION.findall(answer)})


def quality(predicted: list[str], expected: list[str]) -> dict:
    predicted_set, expected_set = set(predicted), set(expected)
    true_positive = len(predicted_set & expected_set)
    return {
        "exact": predicted_set == expected_set,
        "precision": true_positive / len(predicted_set) if predicted_set else 0.0,
        "recall": true_positive / len(expected_set),
        "false_positives": sorted(predicted_set - expected_set),
        "false_negatives": sorted(expected_set - predicted_set),
    }


def score_cell(cell: dict, oracles: dict[str, dict]) -> dict:
    required = ("task_id", "model", "arm", "repetition", "order", "status")
    missing = [field for field in required if field not in cell]
    if missing:
        raise AnalysisError(f"cell missing required fields: {', '.join(missing)}")
    oracle = oracles.get(cell["model"])
    if oracle is None or cell["task_id"] != oracle["task_id"]:
        raise AnalysisError("cell has no matching frozen model/task fixture")
    base = {field: cell[field] for field in required}
    base.update({"schema": SCORE_SCHEMA, "run_key": cell.get("run_key") or cell.get("cell_key"),
                 "completed": cell["status"] == ANSWERED, "failure": cell.get("failure")})
    if cell["status"] != ANSWERED:
        return {**base, "extracted": None, "exact": None, "precision": None,
                "recall": None, "false_positives": None, "false_negatives": None}
    answer = cell.get("final_answer")
    if not isinstance(answer, str) or not answer.strip():
        raise AnalysisError("answered cell must contain a non-empty final_answer")
    found = extract(answer)
    return {**base, "extracted": found, **quality(found, oracle["expected"])}


def _binomial_cdf(k: int, n: int, p: float) -> float:
    return sum(math.comb(n, i) * p**i * (1 - p) ** (n - i) for i in range(k + 1))


def _solve_cdf(k: int, n: int, target: float) -> float:
    low, high = 0.0, 1.0
    for _ in range(80):
        middle = (low + high) / 2
        if _binomial_cdf(k, n, middle) > target:
            low = middle
        else:
            high = middle
    return (low + high) / 2


def exact_binomial_ci(successes: int, n: int, alpha: float = 0.05) -> list[float] | None:
    if n == 0:
        return None
    lower = 0.0 if successes == 0 else _solve_cdf(successes - 1, n, 1 - alpha / 2)
    upper = 1.0 if successes == n else _solve_cdf(successes, n, alpha / 2)
    return [lower, upper]


def exact_mcnemar(b: int, c: int) -> float:
    discordant = b + c
    if discordant == 0:
        return 1.0
    tail = sum(math.comb(discordant, i) for i in range(min(b, c) + 1)) / 2**discordant
    return min(1.0, 2 * tail)


def holm_adjust(p_values: dict[tuple[str, str], float]) -> dict[tuple[str, str], float]:
    ordered = sorted(p_values, key=lambda key: (p_values[key], key))
    adjusted, running, count = {}, 0.0, len(ordered)
    for rank, key in enumerate(ordered):
        running = max(running, min(1.0, (count - rank) * p_values[key]))
        adjusted[key] = running
    return adjusted


def planned_cells(manifest: dict, schedule: dict) -> dict[tuple[str, str, int, str], dict]:
    models = [item["id"] for item in manifest["models"]]
    arms = [item["id"] for item in manifest["arms"]]
    expected = {}
    for block in schedule["blocks"]:
        row = schedule["arm_order_rows"][block["arm_order_row"] - 1]
        for model in block["model_order"]:
            if model not in models:
                raise AnalysisError("schedule contains undeclared model")
            for order, arm in enumerate(row, 1):
                if arm not in arms:
                    raise AnalysisError("schedule contains undeclared arm")
                key = ("search-callsite-v2", model, block["block"], arm)
                expected[key] = {"task_id": key[0], "model": model, "repetition": block["block"],
                                 "arm": arm, "order": order, "cycle": block["cycle"]}
    declared = manifest["schedule"]["total_planned_cells"]
    if len(expected) != declared or len(expected) != schedule.get("total_cells") or len(expected) != 64:
        raise AnalysisError("declared grid size differs from schedule")
    return expected


def validate_preflight(preflight: dict, manifest: dict, oracles: dict) -> dict:
    if preflight.get("schema") != "lci.api-replay.format-plan.v1":
        raise AnalysisError("wrong preflight schema")
    if preflight.get("provider_executed") is not False or preflight.get("cells") != 64:
        raise AnalysisError("preflight must be a provider-free validation of all 64 cells")
    fixtures = preflight.get("fixtures")
    models = {item["id"] for item in manifest["models"]}
    arms = {item["id"] for item in manifest["arms"]}
    if not isinstance(fixtures, list) or len(fixtures) != 2:
        raise AnalysisError("preflight must validate exactly two model fixtures")
    if {item.get("recorded_model") for item in fixtures} != models:
        raise AnalysisError("preflight fixtures do not match declared models")
    for fixture in fixtures:
        model = fixture.get("recorded_model")
        if fixture.get("fixture_digest") != oracles[model]["fixture_digest"]:
            raise AnalysisError(f"preflight fixture digest mismatch for {model}")
        if fixture.get("task_id") != "search-callsite-v2" or set(fixture.get("arms", {})) != arms:
            raise AnalysisError("preflight fixture task or arms do not match the manifest")
        for arm, details in fixture["arms"].items():
            expected = [] if arm == BASELINE else ["/messages/3/content"]
            if details.get("diff_pointers") != expected:
                raise AnalysisError(f"preflight request-isolation failure for {arm}")
            if not isinstance(details.get("request_digest"), str) or not isinstance(details.get("content_digest"), str):
                raise AnalysisError("preflight is missing request/content digests")
    return {"passed": True, "fixture_digests": {item["recorded_model"]: item["fixture_digest"] for item in fixtures}}


def resolve_attempts(cells: list[dict], expected: dict) -> tuple[dict, dict]:
    attempts = defaultdict(list)
    for index, cell in enumerate(cells):
        key = (cell.get("task_id"), cell.get("model"), cell.get("repetition"), cell.get("arm"))
        if key not in expected:
            raise AnalysisError(f"undeclared logical cell {key!r}")
        if cell.get("order") != expected[key]["order"]:
            raise AnalysisError(f"schedule order mismatch for {key!r}")
        attempts[key].append((cell.get("attempt", index + 1), cell))
    selected, retry_counts = {}, {}
    for key, records in attempts.items():
        records.sort(key=lambda item: item[0])
        answers = [cell for _, cell in records if cell.get("status") == ANSWERED]
        if len(answers) > 1:
            raise AnalysisError(f"answered cell was retried: {key!r}")
        selected[key] = answers[0] if answers else records[-1][1]
        retry_counts[key] = len(records) - 1
    return selected, retry_counts


def _summary(scores: list[dict], planned_n: int) -> dict:
    answered = [score for score in scores if score["completed"]]
    exact_n = sum(score["exact"] for score in answered)
    return {"planned_n": planned_n, "observed_n": len(scores), "usable_n": len(answered),
            "unusable_n": len(scores) - len(answered), "missing_n": planned_n - len(scores),
            "completion_rate": len(answered) / planned_n, "exact_n": exact_n,
            "exact_rate": exact_n / len(answered) if answered else None,
            "exact_binomial_95_ci": exact_binomial_ci(exact_n, len(answered)),
            "precision": statistics.mean(s["precision"] for s in answered) if answered else None,
            "recall": statistics.mean(s["recall"] for s in answered) if answered else None}


def analyze(document: dict, oracles: dict, manifest: dict, schedule: dict, preflight: dict) -> dict:
    cells = document.get("cells")
    if not isinstance(cells, list):
        raise AnalysisError("input must contain a cells array")
    expected = planned_cells(manifest, schedule)
    manipulation = validate_preflight(preflight, manifest, oracles)
    selected, retries = resolve_attempts(cells, expected)
    scores = [score_cell(cell, oracles) for cell in selected.values()]
    grouped = defaultdict(list)
    for score in scores:
        grouped[(score["model"], score["arm"])].append(score)
    models = [item["id"] for item in manifest["models"]]
    arms = [item["id"] for item in manifest["arms"]]
    summaries = {model: {arm: _summary(grouped[(model, arm)], 8) for arm in arms} for model in models}

    raw_p, contrasts = {}, {model: {} for model in models}
    for model in models:
        by_key = {(s["repetition"], s["arm"]): s for s in scores if s["model"] == model}
        for arm in arms[1:]:
            usable_pairs = [(by_key[(block, BASELINE)], by_key[(block, arm)]) for block in range(1, 9)
                            if (block, BASELINE) in by_key and (block, arm) in by_key
                            and by_key[(block, BASELINE)]["completed"] and by_key[(block, arm)]["completed"]]
            b = sum(control["exact"] and not candidate["exact"] for control, candidate in usable_pairs)
            c = sum(not control["exact"] and candidate["exact"] for control, candidate in usable_pairs)
            p = exact_mcnemar(b, c)
            raw_p[(model, arm)] = p
            contrasts[model][arm] = {"vs": BASELINE, "usable_pairs": len(usable_pairs),
                "unusable_or_missing_pairs": 8 - len(usable_pairs), "baseline_only_exact": b,
                "candidate_only_exact": c, "paired_exact_delta": (c - b) / len(usable_pairs) if usable_pairs else None,
                "mcnemar_exact_two_sided_p": p,
                "completion_rate_delta": summaries[model][arm]["completion_rate"] - summaries[model][BASELINE]["completion_rate"]}
    adjusted = holm_adjust(raw_p)
    for key, value in adjusted.items():
        contrasts[key[0]][key[1]]["holm_adjusted_p"] = value

    cycles = {}
    for model in models:
        exact_by_cycle = []
        for cycle in (1, 2):
            values = [s for s in scores if s["model"] == model and s["arm"] == BASELINE
                      and expected[(s["task_id"], model, s["repetition"], BASELINE)]["cycle"] == cycle
                      and s["completed"]]
            exact_by_cycle.append(sum(s["exact"] for s in values) / len(values) if len(values) == 4 else None)
        delta = None if None in exact_by_cycle else exact_by_cycle[1] - exact_by_cycle[0]
        cycles[model] = {"cycle_1_exact_rate": exact_by_cycle[0], "cycle_2_exact_rate": exact_by_cycle[1],
                         "delta": delta, "passes": delta is not None and abs(delta) < 0.25}
    manipulation_passed = manipulation["passed"]
    null_passed = all(item["passes"] for item in cycles.values())
    missing = [details for key, details in expected.items() if key not in selected]
    unusable = [{"key": list(key), "status": cell["status"], "failure": cell.get("failure")}
                for key, cell in selected.items() if cell["status"] != ANSWERED]
    grid_complete = not missing and not unusable
    retry_details = [{"key": list(key), "replacement_attempts": count,
                      "attempts": [{"attempt": cell.get("attempt"), "status": cell.get("status"),
                                    "failure": cell.get("failure")}
                                   for cell in cells
                                   if (cell.get("task_id"), cell.get("model"), cell.get("repetition"),
                                       cell.get("arm")) == key]}
                     for key, count in retries.items() if count]
    qualifiers = []
    for arm in arms[1:]:
        positive = any(contrasts[m][arm]["holm_adjusted_p"] < 0.05 and
                       contrasts[m][arm]["paired_exact_delta"] is not None and
                       contrasts[m][arm]["paired_exact_delta"] >= 0.25 for m in models)
        no_harm = all(contrasts[m][arm]["paired_exact_delta"] is not None and
                      contrasts[m][arm]["paired_exact_delta"] > -0.25 for m in models)
        completion = all(contrasts[m][arm]["completion_rate_delta"] >= -0.125 for m in models)
        all_usable = all(summaries[m][arm]["usable_n"] == 8 for m in models)
        if (positive and no_harm and completion and all_usable and null_passed
                and manipulation_passed and grid_complete):
            qualifiers.append(arm)
    return {"schema": SCHEMA, "exploratory": True, "grid_complete": grid_complete,
            "planned_cells": 64, "observed_logical_cells": len(selected), "missing_cells": missing,
            "unusable_cells": unusable, "retry_attempts": sum(retries.values()),
            "retry_counts": {"|".join(map(str, key)): value for key, value in retries.items() if value},
            "retries": retry_details,
            "scores": scores, "by_model_and_arm": summaries, "paired_contrasts_by_model": contrasts,
            "cycle_null": cycles, "manipulation_control": manipulation,
            "multiplicity": {"method": "Holm step-down", "family_size": 6, "alpha": 0.05},
            "selection": {"advanced": qualifiers, "decision": "advance" if qualifiers else "retain fmt_07",
            "production_recommendation": False}}


def load_cells(path: Path) -> dict:
    """Load an exported document or every immutable attempt in a result directory."""
    if path.is_dir():
        attempts = path / "attempts"
        records = [json.loads(item.read_text()) for item in sorted(attempts.glob("*.json"))]
        return {"cells": records}
    return json.loads(path.read_text())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cells", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--schedule", type=Path, required=True)
    parser.add_argument("--preflight", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    result = analyze(load_cells(args.cells), load_oracles(args.fixtures),
                     json.loads(args.manifest.read_text()), json.loads(args.schedule.read_text()),
                     json.loads(args.preflight.read_text()))
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.out:
        args.out.write_text(rendered)
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
