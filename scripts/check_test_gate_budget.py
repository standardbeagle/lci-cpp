#!/usr/bin/env python3
"""Fail closed unless recorded warm full gates satisfy their stated budget."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


REQUIRED_ENVIRONMENT = (
    "git_commit",
    "cmake_preset",
    "cxx_compiler",
    "cpu_count",
    "ctest_parallelism",
    "gate_command",
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "evidence",
        nargs="?",
        type=Path,
        default=Path("docs/performance/test-suite-final.json"),
    )
    args = parser.parse_args()
    try:
        data = json.loads(args.evidence.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print(f"test-gate-budget: invalid evidence: {exc}", file=sys.stderr)
        return 2

    missing = [key for key in REQUIRED_ENVIRONMENT if not data.get("environment", {}).get(key)]
    if missing:
        print(f"test-gate-budget: missing comparison metadata: {', '.join(missing)}", file=sys.stderr)
        return 2

    runs = data.get("gate_runs", [])
    minimum = data.get("policy", {}).get("minimum_consecutive_runs")
    budget = data.get("policy", {}).get("total_wall_budget_s")
    if not isinstance(minimum, int) or not isinstance(budget, (int, float)):
        print("test-gate-budget: missing or invalid policy", file=sys.stderr)
        return 2
    if len(runs) < minimum:
        print(f"test-gate-budget: need {minimum} consecutive runs; found {len(runs)}", file=sys.stderr)
        return 2

    failures = []
    for run in runs:
        index = run.get("run_index", "?")
        total = run.get("total_wall_s")
        if run.get("build_exit") != 0 or run.get("ctest_exit") != 0 or run.get("tests_failed") != 0:
            failures.append(f"run {index} is not green")
        if not isinstance(total, (int, float)) or total >= budget:
            failures.append(f"run {index} total {total!r}s is not below {budget}s")

    if failures:
        for failure in failures:
            print(f"test-gate-budget: FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"test-gate-budget: PASS: {len(runs)} green consecutive gates below {budget}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
