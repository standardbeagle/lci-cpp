#!/usr/bin/env python3
"""Aggregate judged benchmark results into a markdown report + CSV.

Usage:
  report.py --dir results/tier0 [--out results/tier0/report.md]
"""

import argparse
import csv
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchlib as bl


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else None


def fmt(x, nd=2):
    return "-" if x is None else (f"{x:,.0f}" if nd == 0 else f"{x:.{nd}f}")


def collect(run_dir):
    cfg = bl.Config()
    rows = []
    for _, res in bl.iter_results(run_dir):
        s = res.get("scores", {})
        j = s.get("judge", {})
        tok = res["tokens"]
        rows.append({
            "repo": res["repo"], "model": res["model"], "variant": res["variant"],
            "minefield": cfg.repos.get(res["repo"], {}).get("minefield-tier", "?"),
            "question_id": res["question_id"], "difficulty": res["difficulty"],
            "rep": res["rep"], "status": res["status"],
            "wall_seconds": res["wall_seconds"],
            "tokens_in": tok["input"], "tokens_out": tok["output"],
            "tokens_cache_read": tok["cache_read"],
            "tokens_total": tok["input"] + tok["output"] + tok["cache_read"] + tok["cache_write"],
            "cost": res.get("cost", 0), "steps": res["steps"],
            "tool_calls": len(res["tool_calls"]),
            "lci_tool_calls": sum(1 for t in res["tool_calls"] if t.startswith("lci_")),
            "fact_accuracy": s.get("fact_accuracy"),
            "tool_match": s.get("tool_match"),
            "judge_correctness": j.get("correctness"),
            "judge_completeness": j.get("completeness"),
            "judge_grounding": j.get("grounding"),
            "judge_conciseness": j.get("conciseness"),
            "judge_overall": j.get("overall"),
        })
    return rows


def agg_table(rows, keys):
    groups = defaultdict(list)
    for r in rows:
        groups[tuple(r[k] for k in keys)].append(r)
    out = []
    for gk in sorted(groups):
        g = groups[gk]
        ok = [r for r in g if r["status"] == "ok"]
        out.append({
            **dict(zip(keys, gk)),
            "n": len(g), "ok": len(ok),
            "facts": mean([r["fact_accuracy"] for r in g]),
            "tool_match": mean([r["tool_match"] for r in g]),
            "judge": mean([r["judge_overall"] for r in g]),
            "grounding": mean([r["judge_grounding"] for r in g]),
            "tok_in": mean([r["tokens_in"] for r in ok]),
            "tok_out": mean([r["tokens_out"] for r in ok]),
            "tok_cache": mean([r["tokens_cache_read"] for r in ok]),
            "tok_total": mean([r["tokens_total"] for r in ok]),
            "cost": mean([r["cost"] for r in ok]),
            "wall": mean([r["wall_seconds"] for r in ok]),
            "tools": mean([r["tool_calls"] for r in ok]),
        })
    return out


def md_table(agg, keys):
    hdr = keys + ["n", "ok", "facts", "tool_match", "judge", "grounding", "tok_total", "tok_in", "tok_out", "tok_cache", "cost$", "wall_s", "tools"]
    lines = ["| " + " | ".join(hdr) + " |", "|" + "---|" * len(hdr)]
    for a in agg:
        cells = [str(a[k]) for k in keys] + [
            str(a["n"]), str(a["ok"]), fmt(a["facts"]), fmt(a["tool_match"]), fmt(a["judge"]),
            fmt(a["grounding"]), fmt(a["tok_total"], 0), fmt(a["tok_in"], 0),
            fmt(a["tok_out"], 0), fmt(a["tok_cache"], 0),
            fmt(a["cost"], 4), fmt(a["wall"], 1), fmt(a["tools"], 1),
        ]
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--out")
    args = ap.parse_args()
    run_dir = args.dir if os.path.isabs(args.dir) else os.path.join(bl.BENCH_ROOT, args.dir)
    rows = collect(run_dir)
    if not rows:
        sys.exit("no results found")

    csv_path = os.path.join(run_dir, "results.csv")
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    sections = [
        ("Model x Variant", ["model", "variant"]),
        ("Minefield tier x Variant", ["minefield", "variant"]),
        ("Model x Variant x Difficulty", ["model", "variant", "difficulty"]),
        ("Repo x Variant", ["repo", "variant"]),
    ]
    parts = [f"# Repo-QA benchmark report\n\nRuns: {len(rows)} "
             f"(ok: {sum(1 for r in rows if r['status'] == 'ok')})\n"]
    for title, keys in sections:
        parts.append(f"## {title}\n\n{md_table(agg_table(rows, keys), keys)}\n")

    failed = [r for r in rows if r["status"] != "ok"]
    if failed:
        parts.append("## Failed runs\n")
        for r in failed:
            parts.append(f"- {r['repo']} {r['model']} {r['variant']} {r['question_id']} r{r['rep']}: {r['status']}")

    report = "\n".join(parts) + "\n"
    out_path = args.out or os.path.join(run_dir, "report.md")
    if not os.path.isabs(out_path):
        out_path = os.path.join(bl.BENCH_ROOT, out_path)
    with open(out_path, "w") as f:
        f.write(report)
    print(report)
    print(f"written: {out_path}, {csv_path}")


if __name__ == "__main__":
    main()
