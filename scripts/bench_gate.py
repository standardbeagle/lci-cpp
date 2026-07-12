#!/usr/bin/env python3
"""Performance regression gate over google-benchmark JSON output.

Compares a fresh benchmark run against a checked-in baseline and fails
when any benchmark regresses beyond the threshold. Replaces the deleted
Go-oracle bench_parity.py with an absolute-baseline gate.

Check (CI):
    lci_benchmarks --benchmark_filter=-RealProject \
        --benchmark_repetitions=3 --benchmark_report_aggregates_only=true \
        --benchmark_out=benchmark_results.json --benchmark_out_format=json
    scripts/bench_gate.py --baseline tests/benchmarks/baseline/linux-x64.json \
        --current benchmark_results.json

Update baseline (run on the CI runner machine, commit the result):
    scripts/bench_gate.py --baseline tests/benchmarks/baseline/linux-x64.json \
        --current benchmark_results.json --update

Exit codes: 0 ok, 1 regression/mismatch, 2 usage or missing input.
"""

import argparse
import json
import sys

BASELINE_FORMAT = 1

UNIT_TO_NS = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}


def fail(msg: str, code: int = 1) -> None:
    print(f"bench_gate: FAIL: {msg}", file=sys.stderr)
    sys.exit(code)


def load_json(path: str, what: str):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        fail(
            f"{what} not found: {path}\n"
            "  Generate one on the benchmark runner:\n"
            "    lci_benchmarks --benchmark_filter=-RealProject "
            "--benchmark_repetitions=3 --benchmark_report_aggregates_only=true "
            "--benchmark_out=current.json --benchmark_out_format=json\n"
            f"    scripts/bench_gate.py --baseline {path} --current current.json --update",
            code=2,
        )
    except json.JSONDecodeError as e:
        fail(f"{what} is not valid JSON ({path}): {e}", code=2)


def extract_times_ns(run: dict) -> dict:
    """Map benchmark name -> real_time in ns from google-benchmark JSON.

    Prefers the median aggregate when repetitions were used; errors on
    error_occurred entries so skipped/broken benchmarks can't pass silently.
    """
    entries = run.get("benchmarks")
    if not isinstance(entries, list) or not entries:
        fail("current run contains no benchmarks — wrong file or empty run", code=2)

    errored = [e.get("name", "?") for e in entries if e.get("error_occurred")]
    if errored:
        fail(
            "benchmarks reported errors (missing corpus or setup failure):\n  "
            + "\n  ".join(errored)
        )

    have_aggregates = any(e.get("run_type") == "aggregate" for e in entries)
    times: dict = {}
    for e in entries:
        if have_aggregates:
            if e.get("run_type") != "aggregate" or e.get("aggregate_name") != "median":
                continue
            name = e["run_name"]
        else:
            name = e["name"]
        unit = e.get("time_unit", "ns")
        if unit not in UNIT_TO_NS:
            fail(f"unknown time_unit '{unit}' for {name}", code=2)
        times[name] = e["real_time"] * UNIT_TO_NS[unit]
    if not times:
        fail("no comparable entries (aggregates present but no medians?)", code=2)
    return times


def cmd_update(baseline_path: str, current: dict) -> None:
    times = extract_times_ns(current)
    baseline = {
        "format": BASELINE_FORMAT,
        "host": current.get("context", {}).get("host_name", "unknown"),
        "date": current.get("context", {}).get("date", "unknown"),
        "benchmarks": {name: round(ns, 1) for name, ns in sorted(times.items())},
    }
    with open(baseline_path, "w", encoding="utf-8") as f:
        json.dump(baseline, f, indent=2)
        f.write("\n")
    print(f"bench_gate: baseline written to {baseline_path} ({len(times)} benchmarks)")


def cmd_check(baseline: dict, current: dict, threshold: float) -> None:
    if baseline.get("format") != BASELINE_FORMAT:
        fail(f"baseline format {baseline.get('format')} != {BASELINE_FORMAT}", code=2)

    base = baseline["benchmarks"]
    cur = extract_times_ns(current)

    missing = sorted(set(base) - set(cur))
    added = sorted(set(cur) - set(base))
    if missing:
        fail(
            "benchmarks in baseline but absent from current run "
            "(deleted or renamed? update the baseline in the same commit):\n  "
            + "\n  ".join(missing)
        )
    if added:
        fail(
            "benchmarks missing from baseline (new benchmark? re-run with "
            "--update on the runner and commit the baseline):\n  "
            + "\n  ".join(added)
        )

    regressions = []
    improvements = []
    width = max(len(n) for n in base)
    print(f"bench_gate: threshold {threshold:.2f}x, {len(base)} benchmarks")
    for name in sorted(base):
        ratio = cur[name] / base[name] if base[name] > 0 else float("inf")
        marker = ""
        if ratio > threshold:
            marker = "  << REGRESSION"
            regressions.append((name, ratio))
        elif ratio < 1.0 / threshold:
            marker = "  (faster — consider refreshing baseline)"
            improvements.append((name, ratio))
        print(f"  {name:<{width}}  {base[name]:>12.0f}ns -> {cur[name]:>12.0f}ns  {ratio:5.2f}x{marker}")

    if improvements:
        print(f"bench_gate: {len(improvements)} benchmark(s) improved beyond threshold")
    if regressions:
        fail(
            f"{len(regressions)} benchmark(s) regressed beyond {threshold:.2f}x:\n  "
            + "\n  ".join(f"{n}: {r:.2f}x" for n, r in regressions)
        )
    print("bench_gate: OK — no regression beyond threshold")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--baseline", required=True, help="checked-in baseline JSON")
    p.add_argument("--current", required=True, help="google-benchmark --benchmark_out JSON")
    p.add_argument("--threshold", type=float, default=1.5,
                   help="fail when current/baseline exceeds this ratio (default 1.5)")
    p.add_argument("--update", action="store_true",
                   help="rewrite the baseline from the current run instead of checking")
    args = p.parse_args()

    if args.threshold <= 1.0:
        fail("--threshold must be > 1.0", code=2)

    current = load_json(args.current, "current run")
    if args.update:
        cmd_update(args.baseline, current)
    else:
        baseline = load_json(args.baseline, "baseline")
        cmd_check(baseline, current, args.threshold)


if __name__ == "__main__":
    main()
