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
    else:
        seen_arm_ids = set()
        for index, arm in enumerate(arms):
            if not isinstance(arm, dict):
                errors.append(f"arms[{index}] must be an object")
            elif not isinstance(arm.get("id"), str) or not arm["id"]:
                errors.append(f"arms[{index}] is missing a non-empty string id")
            elif arm["id"] in seen_arm_ids:
                errors.append(f"arms[{index}] duplicates arm id: {arm['id']}")
            else:
                seen_arm_ids.add(arm["id"])
    if not isinstance(data.get("repetitions"), int) or data.get("repetitions", 0) < 2:
        errors.append("repetitions must be an integer >= 2")
    metrics = data.get("metrics")
    if not isinstance(metrics, list) or not any(isinstance(m, dict) and m.get("primary") is True for m in metrics):
        errors.append("metrics must declare at least one primary metric")
    controls = data.get("controls")
    if not isinstance(controls, list) or not any(isinstance(c, dict) and c.get("kind") == "null" for c in controls):
        errors.append("controls must declare at least one null control")
    oracle = data.get("oracle")
    if isinstance(oracle, dict):
        for key in ("source", "independence_argument", "good_case", "bad_case"):
            if not oracle.get(key):
                errors.append(f"oracle.{key} is required")
    elif "oracle" in data:
        # One shape error, not four stacked oracle.* errors; a wholly absent
        # oracle is already reported as a missing required field.
        errors.append("oracle must be an object")
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
        ("a missing oracle independence argument is rejected",
         {**minimal, "oracle": {"source": "s", "good_case": "g", "bad_case": "b"}},
         "oracle.independence_argument is required"),
        ("a non-object oracle is one shape error", {**minimal, "oracle": "external"},
         "oracle must be an object"),
        ("a non-object arm is rejected", {**minimal, "arms": [{"id": "a"}, "b"]},
         "arms[1] must be an object"),
        ("an arm without an id is rejected", {**minimal, "arms": [{"id": "a"}, {}]},
         "arms[1] is missing a non-empty string id"),
        ("a duplicate arm id is rejected",
         {**minimal, "arms": [{"id": "a"}, {"id": "a"}]},
         "arms[1] duplicates arm id: a"),
        ("a missing required field is reported", minimal, "missing required field: analysis_plan"),
    ]
    failures = [f"{label}: expected {expected!r} in {sorted(validate(data))}"
                for label, data, expected in cases if expected not in validate(data)]
    non_object = validate([])
    if non_object != ["manifest must be a JSON object"]:
        failures.append(
            f"a non-object manifest must report exactly one shape error, got {non_object}")
    non_object_oracle = [e for e in validate({**minimal, "oracle": "external"}) if "oracle" in e]
    if non_object_oracle != ["oracle must be an object"]:
        failures.append(
            f"a non-object oracle must yield one shape error, got {non_object_oracle}")
    if failures:
        raise SystemExit("validate_manifest self-test failed:\n" + "\n".join(failures))
    print(f"validate_manifest self-test: {len(cases) + 2} checks passed")
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
