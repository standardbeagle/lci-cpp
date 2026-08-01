#!/usr/bin/env python3
"""Validate the structural completeness of a benchmark preregistration."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


REQUIRED = {
    "schema", "benchmark_id", "decision", "claim_boundary", "experimental_unit",
    "population", "arms", "equivalence", "hypotheses", "metrics", "strata",
    "controls", "repetitions", "stopping_rule", "noise_rule",
    "practical_effect_threshold", "exclusions", "failure_policy", "oracle",
    "prompt_neutrality", "leakage_controls", "analysis_plan", "reporting_plan",
    "fixture_digests", "analysis_revision", "registered_at",
}


def validate(data: object) -> list[str]:
    if not isinstance(data, dict):
        return ["manifest must be a JSON object"]
    errors = [f"missing required field: {key}" for key in sorted(REQUIRED - data.keys())]
    if data.get("schema") != "benchmark.preregistration.v1":
        errors.append("schema must be benchmark.preregistration.v1")
    arms = data.get("arms")
    if not isinstance(arms, list) or len(arms) < 2:
        errors.append("arms must contain at least two entries")
    elif len({arm.get("id") for arm in arms if isinstance(arm, dict)}) != len(arms):
        errors.append("arm ids must be present and unique")
    if not isinstance(data.get("repetitions"), int) or data.get("repetitions", 0) < 2:
        errors.append("repetitions must be an integer >= 2")
    metrics = data.get("metrics")
    if not isinstance(metrics, list) or not any(isinstance(m, dict) and m.get("primary") is True for m in metrics):
        errors.append("metrics must declare at least one primary metric")
    controls = data.get("controls")
    if not isinstance(controls, list) or not any(isinstance(c, dict) and c.get("kind") == "null" for c in controls):
        errors.append("controls must declare at least one null control")
    oracle = data.get("oracle")
    for key in ("source", "independence_argument", "good_case", "bad_case"):
        if not isinstance(oracle, dict) or not oracle.get(key):
            errors.append(f"oracle.{key} is required")
    return errors


def self_test() -> int:
    """Assertion-free so the checks still run under `python -O`."""
    minimal = {"schema": "benchmark.preregistration.v1", "repetitions": 1}
    cases = [
        ("a non-object manifest is rejected", [], "manifest must be a JSON object"),
        ("a bad repetition count is rejected", minimal, "repetitions must be an integer >= 2"),
        ("a wrong schema is rejected", {"schema": "other"}, "schema must be benchmark.preregistration.v1"),
        ("fewer than two arms is rejected", minimal, "arms must contain at least two entries"),
        ("a missing primary metric is rejected", minimal, "metrics must declare at least one primary metric"),
        ("a missing null control is rejected", minimal, "controls must declare at least one null control"),
        ("a missing oracle independence argument is rejected", minimal,
         "oracle.independence_argument is required"),
        ("a missing required field is reported", minimal, "missing required field: analysis_plan"),
    ]
    failures = [f"{label}: expected {expected!r} in {sorted(validate(data))}"
                for label, data, expected in cases if expected not in validate(data)]
    if validate([]) and len(validate([])) != 1:
        failures.append("a non-object manifest must report exactly one error")
    if failures:
        raise SystemExit("validate_manifest self-test failed:\n" + "\n".join(failures))
    print(f"validate_manifest self-test: {len(cases) + 1} checks passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path, nargs="?")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.manifest is None:
        parser.error("manifest is required unless --self-test is used")
    try:
        data = json.loads(args.manifest.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print(f"INVALID: {exc}")
        return 2
    errors = validate(data)
    if errors:
        for error in errors:
            print(f"INVALID: {error}")
        return 1
    print("VALID")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
