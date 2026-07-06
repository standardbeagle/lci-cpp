#!/usr/bin/env python3
"""Fine-tooth comb over lci-variant traces: did the four post-tier0 fixes get
exercised, and what waste / new failure patterns remain?

Fixes under test (commit 817ea43):
  A. find_files `**/name` matches root-level files
  B. search validation errors carry allowed_params (recoverable)
  C. find_files/browse_file accept path= alias
  D. get_context emits callers count

Usage: comb.py --dir results/tier1-postfix
"""
import argparse, json, os, re, sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchlib as bl

BASH_HINTS = ("grep", "rg ", "find ", "cat ", "ls ", "head ", "tail ", "awk", "sed")


def is_real_error(msg):
    """Genuine tool error vs the harness `"error"`-substring false positive
    (a successful result that merely mentions error)."""
    return ('"success":false' in msg or '"success": false' in msg
            or 'validation_errors' in msg or '"error":{' in msg)


def load(run_dir):
    runs = []
    for _, r in bl.iter_results(run_dir):
        if r.get("variant") not in ("lci", "lci-slim"):
            continue
        runs.append(r)
    return runs


def is_glob_starstar(args):
    for v in (args or {}).values():
        if isinstance(v, str) and v.startswith("**/"):
            return v
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--examples", type=int, default=4)
    args = ap.parse_args()
    run_dir = args.dir if os.path.isabs(args.dir) else os.path.join(bl.BENCH_ROOT, args.dir)
    runs = load(run_dir)
    traced = [r for r in runs if r.get("tool_trace")]

    print(f"# COMB {run_dir}")
    print(f"lci runs: {len(runs)}  (traced: {len(traced)})\n")

    # ---- Fix A: **/ globs on find_files ----
    ss_calls = []          # (qid, model, pattern, output_chars)
    for r in traced:
        for e in r["tool_trace"]:
            if e["tool"] == "lci_find_files":
                g = is_glob_starstar(e.get("args"))
                if g:
                    ss_calls.append((r["question_id"], r["model"], g, e.get("output_chars", 0)))
    ss_hit = [c for c in ss_calls if c[3] >= 80]
    print(f"## A. find_files **/ globs: {len(ss_calls)} calls, {len(ss_hit)} returned content (>=80ch)")
    for c in ss_calls[:args.examples]:
        print(f"    {c[1]} {c[0]}: {c[2]!r} -> {c[3]}ch {'HIT' if c[3]>=80 else 'EMPTY'}")

    # ---- all find_files calls: content rate ----
    ff = [(r, e) for r in traced for e in r["tool_trace"] if e["tool"] == "lci_find_files"]
    ff_hit = [x for x in ff if x[1].get("output_chars", 0) >= 80]
    print(f"\n## find_files overall: {len(ff)} calls, {len(ff_hit)} returned content")

    # ---- Fix C: path= alias on find_files/browse_file ----
    alias = []
    for r in traced:
        for e in r["tool_trace"]:
            if e["tool"] in ("lci_find_files", "lci_browse_file"):
                a = e.get("args") or {}
                if "path" in a:
                    alias.append((r["question_id"], r["model"], e["tool"], a.get("path"),
                                  e.get("output_chars", 0), bool(e.get("tool_error"))))
    print(f"\n## C. path= alias on find_files/browse_file: {len(alias)} uses")
    for c in alias[:args.examples]:
        print(f"    {c[1]} {c[0]} {c[2]} path={c[3]!r} -> {c[4]}ch err={c[5]}")

    # ---- Fix B: search param errors + recovery ----
    search_errs = []       # (qid, model, args, msg, recovered)
    for r in traced:
        tr = r["tool_trace"]
        for i, e in enumerate(tr):
            if e["tool"] == "lci_search" and e.get("tool_error"):
                msg = e["tool_error"]
                names_allowed = "allowed" in msg.lower() or "allowed_params" in msg
                # recovery = a later successful lci_search in same run
                recovered = any(
                    tr[j]["tool"] == "lci_search" and not tr[j].get("tool_error")
                    and tr[j].get("output_chars", 0) >= 80
                    for j in range(i + 1, len(tr)))
                search_errs.append((r["question_id"], r["model"],
                                    e.get("args"), names_allowed, recovered, msg[:200]))
    real = [x for x in search_errs if is_real_error(x[5])]
    false_pos = len(search_errs) - len(real)
    named = sum(1 for x in real if x[3])
    recov = sum(1 for x in real if x[4])
    print(f"\n## B. search flagged-error payloads: {len(search_errs)} "
          f"({false_pos} false-pos '\"error\"'-substring, {len(real)} genuine; "
          f"of genuine: {named} carried allowed-list, {recov} recovered later in-run)")
    search_errs = real
    for c in search_errs[:args.examples]:
        print(f"    {c[1]} {c[0]}: args={c[2]} named_allowed={c[3]} recovered={c[4]}")
        print(f"        msg: {c[5]}")

    # ---- Fix D: get_context callers count consumed ----
    gc = [(r, e) for r in traced for e in r["tool_trace"] if e["tool"] == "lci_get_context"]
    print(f"\n## D. get_context calls: {len(gc)}  "
          f"(callers field verified by direct probe; output_chars distribution below)")
    # search callers inline: count searches
    srch = [(r, e) for r in traced for e in r["tool_trace"] if e["tool"] == "lci_search"]
    print(f"    lci_search calls: {len(srch)}")

    # ---- waste: near-empty nav outputs ----
    empties = Counter()
    empty_ex = []
    for r in traced:
        for e in r["tool_trace"]:
            t = e["tool"]
            if t.startswith("lci_") and not e.get("tool_error") and e.get("output_chars", 0) < 80:
                empties[t] += 1
                if len(empty_ex) < 8:
                    empty_ex.append((r["question_id"], r["model"], t, e.get("args"), e.get("output_chars", 0)))
    print(f"\n## Waste: near-empty lci outputs (<80ch, no error): {dict(empties) or 'none'}")
    for c in empty_ex[:args.examples]:
        print(f"    {c[1]} {c[0]} {c[2]} args={c[3]} -> {c[4]}ch")

    # ---- waste: all tool_error payloads by tool ----
    errs = Counter()
    err_ex = []
    for r in traced:
        for e in r["tool_trace"]:
            if e.get("tool_error") and is_real_error(e["tool_error"]):
                errs[e["tool"]] += 1
                if len(err_ex) < 10:
                    err_ex.append((r["question_id"], r["model"], e["tool"], e["tool_error"][:180]))
    print(f"\n## Genuine tool-level error payloads: {dict(errs) or 'none'}")
    for c in err_ex:
        print(f"    {c[1]} {c[0]} {c[2]}: {c[3]}")

    # ---- waste: bash fallback after an lci call ----
    bash_after = 0
    bash_ex = []
    for r in traced:
        seen_lci = False
        counted = False
        for e in r["tool_trace"]:
            if e["tool"].startswith("lci_"):
                seen_lci = True
            elif e["tool"] == "bash" and seen_lci and not counted:
                a = e.get("args") or {}
                cmd = str(a.get("command", ""))
                if any(h in cmd for h in BASH_HINTS):
                    bash_after += 1
                    counted = True
                    if len(bash_ex) < 6:
                        bash_ex.append((r["question_id"], r["model"], cmd[:120]))
    print(f"\n## Waste: bash-explore fallback after an lci call: {bash_after}/{len(traced)} runs")
    for c in bash_ex[:args.examples]:
        print(f"    {c[1]} {c[0]}: {c[2]}")

    # ---- waste: repeated reads of same file (paged re-reads) ----
    reread_runs = 0
    total_reads = 0
    for r in traced:
        files = Counter()
        for e in r["tool_trace"]:
            if e["tool"] == "read":
                total_reads += 1
                fp = (e.get("args") or {}).get("filePath") or (e.get("args") or {}).get("path")
                if fp:
                    files[fp] += 1
        if any(c > 1 for c in files.values()):
            reread_runs += 1
    print(f"\n## Waste: reads={total_reads}; runs with same-file re-read: {reread_runs}/{len(traced)}")

    # ---- refinement (broad->narrow) ----
    refine = 0
    for r in traced:
        seen = defaultdict(set)
        for e in r["tool_trace"]:
            t = e["tool"]
            if t.startswith("lci_"):
                key = json.dumps(e.get("args") or {}, sort_keys=True)
                if seen[t] and key not in seen[t]:
                    refine += 1
                seen[t].add(key)
    print(f"\n## Refinement re-calls (same lci tool, different args): {refine}")

    # ---- call mix ----
    mix = Counter()
    for r in runs:
        for t in r["tool_calls"]:
            mix[t] += 1
    print(f"\n## Call mix (all lci runs): {dict(mix.most_common(12))}")


if __name__ == "__main__":
    main()
