#!/usr/bin/env python3
"""Profile benchmark corpora by architectural-minefield metrics, using LCI.

The benchmark's repo-difficulty axis is not LOC — it is how architecturally
treacherous the code is: call-graph chokepoints, wildly variable function
complexity, cross-module coupling, cyclic tangles, side-effect-heavy code,
and (lack of) modularity. LCI computes these (Brandes betweenness, Louvain
modularity, Tarjan cycles, module cohesion/coupling, purity); this script
turns them into a per-repo profile plus a composite minefield index.

Usage: profile_repos.py [--repo chi ...]     # default: all repos in config
Writes repo-profiles.json and prints a ranked table.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchlib as bl
from tooleval import McpSession


def grab(pattern, text, cast=float, default=None):
    m = re.search(pattern, text)
    return cast(m.group(1)) if m else default


def profile_one(lci_bin, corpus):
    s = McpSession(lci_bin, corpus)
    try:
        unified, _ = s.call_tool("code_insight", {"mode": "unified"})
        t = unified["text"]
    finally:
        s.close()

    p = {
        "files": grab(r"files=(\d+)", t, int),
        "symbols": grab(r"symbols=(\d+)", t, int),
        "complexity_avg": grab(r"complexity: avg=([\d.]+)", t),
        "max_cc": grab(r"cc=([\d.]+)", t, float, 0.0),
        "coupling_avg": grab(r"coupling: avg=([\d.]+)", t),
        "cohesion_avg": grab(r"cohesion: avg=([\d.]+)", t),
        "modularity": grab(r"modularity=([\d.]+)", t),
        "communities": grab(r"communities=(\d+)", t, int),
        "cycles": grab(r"== CYCLES ==\ncount=(\d+)", t, int, 0),
        "layer_violations": grab(r"== LAYER VIOLATIONS ==\ncount=(\d+)", t, int, 0),
        "purity_ratio": grab(r"ratio=([\d.]+)", t),
        "maintainability": grab(r"maintainability=([\d.]+)", t),
        "problematic_symbols": len(re.findall(r"risk=\d+", t)),
        "max_risk": max((int(x) for x in re.findall(r"risk=(\d+)", t)), default=0),
        "broker_max_betweenness": max(
            (float(x) for x in re.findall(r"betweenness=([\d.]+)", t)), default=0.0),
    }
    # Chokepoint concentration: how much of the graph the single most
    # load-bearing symbol reaches. High = one function everything routes
    # through — precisely a minefield.
    reaches = [int(x) for x in re.findall(r"reach=(\d+)", t)]
    p["top_reach_share"] = round(max(reaches) / p["symbols"], 3) if reaches and p["symbols"] else 0.0

    # Composite 0-100. Transparent equal-weight mean of bounded subscores;
    # each term is "higher = more treacherous".
    subs = [
        p["coupling_avg"] or 0,
        1 - (p["cohesion_avg"] or 0),
        1 - (p["modularity"] or 0),
        min(p["cycles"], 20) / 20,
        min(p["layer_violations"], 20) / 20,
        1 - (p["purity_ratio"] or 0),
        min(p["max_cc"], 60) / 60,
        min(p["problematic_symbols"], 10) / 10,
        p["top_reach_share"],
        p["broker_max_betweenness"],
    ]
    p["minefield_index"] = round(100 * sum(subs) / len(subs), 1)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", action="append")
    args = ap.parse_args()
    cfg = bl.Config()
    repos = args.repo or list(cfg.repos)

    profiles = {}
    for repo in repos:
        corpus = os.path.join(cfg.defaults["corpus-root"], cfg.repos[repo]["path"])
        print(f"profiling {repo} ...", flush=True)
        profiles[repo] = profile_one(cfg.defaults["lci-bin"], corpus)

    out = os.path.join(bl.BENCH_ROOT, "repo-profiles.json")
    bl.write_json_atomic(out, profiles)

    cols = ["minefield_index", "max_cc", "cycles", "layer_violations",
            "coupling_avg", "cohesion_avg", "modularity", "purity_ratio",
            "top_reach_share", "problematic_symbols", "symbols"]
    print(f"\n{'repo':12}" + "".join(f"{c[:12]:>13}" for c in cols))
    for repo, p in sorted(profiles.items(), key=lambda kv: -kv[1]["minefield_index"]):
        print(f"{repo:12}" + "".join(
            f"{(p[c] if p[c] is not None else '-'):>13}" for c in cols))
    print(f"\nwritten: {out}")


if __name__ == "__main__":
    main()
