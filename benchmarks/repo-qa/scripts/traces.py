#!/usr/bin/env python3
"""Trace-level LCI evaluation: is LCI actually used, do calls fail, do they
need follow-up?

Reads result files (rich analysis needs `tool_trace` from runs made after
trace capture landed; older results degrade to name-level stats).

Per model x variant it reports:
- adoption: runs with >=1 lci_* call / runs where LCI was available
- lci vs bash call mix
- failed calls: transport errors + tool-level error payloads, by tool
- follow-up patterns: same lci tool re-called with different args in one run
  (refinement/retry), and bash fallback issued after an lci call (LCI answer
  didn't satisfy)
- zero-signal calls: lci calls returning trivially small output (<80 chars)

Usage: traces.py --dir results/traces [--examples]
"""

import argparse
import os
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchlib as bl

EXPLORE_BASH_HINTS = ("grep", "find ", "cat ", "rg ", "ls ", "head ", "tail ")


def analyze(run_dir, show_examples):
    groups = defaultdict(list)
    for _, res in bl.iter_results(run_dir):
        groups[(res["model"], res["variant"])].append(res)

    for (model, variant), runs in sorted(groups.items()):
        n = len(runs)
        traced = [r for r in runs if r.get("tool_trace")]
        lci_runs = [r for r in runs if any(t.startswith("lci_") for t in r["tool_calls"])]
        calls = Counter()
        for r in runs:
            for t in r["tool_calls"]:
                calls[t] += 1
        lci_calls = sum(v for k, v in calls.items() if k.startswith("lci_"))
        bash_calls = calls.get("bash", 0)

        print(f"\n== {model} / {variant}  ({n} runs, {len(traced)} with traces)")
        if variant != "base":
            print(f"  adoption: {len(lci_runs)}/{n} runs used lci tools; "
                  f"calls: lci={lci_calls} bash={bash_calls} read={calls.get('read', 0)}")
            top = ", ".join(f"{k}={v}" for k, v in calls.most_common(6))
            print(f"  call mix: {top}")

        if not traced:
            print("  (no tool_trace stored — re-run for failure/follow-up analysis)")
            continue

        failed = Counter()
        tool_errors = Counter()
        zero_signal = Counter()
        refine = 0
        bash_after_lci = 0
        examples = []
        for r in traced:
            tr = r["tool_trace"]
            seen_args = defaultdict(set)
            lci_seen = False
            bash_fallback_counted = False
            for e in tr:
                tool = e["tool"]
                if e.get("status") not in ("completed", "?") or e.get("error"):
                    failed[tool] += 1
                    if len(examples) < 6:
                        examples.append((r["question_id"], tool,
                                         (e.get("error") or e.get("status", ""))[:160]))
                if e.get("tool_error"):
                    tool_errors[tool] += 1
                    if len(examples) < 6:
                        examples.append((r["question_id"], tool, e["tool_error"][:160]))
                if tool.startswith("lci_"):
                    lci_seen = True
                    if e.get("output_chars", 0) < 80 and not e.get("error"):
                        zero_signal[tool] += 1
                    key = str(sorted((e.get("args") or {}).items()))
                    if seen_args[tool] and key not in seen_args[tool]:
                        refine += 1
                    seen_args[tool].add(key)
                elif tool == "bash" and lci_seen and not bash_fallback_counted:
                    bash_after_lci += 1
                    bash_fallback_counted = True

        total_lci = sum(1 for r in traced for e in r["tool_trace"] if e["tool"].startswith("lci_"))
        print(f"  failed calls: {dict(failed) or 'none'};"
              f" tool-level error payloads: {dict(tool_errors) or 'none'}")
        print(f"  follow-up: {refine} refinement re-calls;"
              f" {bash_after_lci}/{len(traced)} runs fell back to bash after lci")
        print(f"  zero-signal lci calls (<80 chars out): {dict(zero_signal) or 'none'}"
              f"  (of {total_lci} lci calls)")
        if show_examples and examples:
            print("  examples:")
            for qid, tool, msg in examples:
                print(f"    {qid} {tool}: {msg}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--examples", action="store_true")
    args = ap.parse_args()
    run_dir = args.dir if os.path.isabs(args.dir) else os.path.join(bl.BENCH_ROOT, args.dir)
    analyze(run_dir, args.examples)


if __name__ == "__main__":
    main()
