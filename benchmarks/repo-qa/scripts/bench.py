#!/usr/bin/env python3
"""Repo-QA benchmark runner.

Runs opencode (baseline vs +LCI MCP) against question banks, capturing
answer text, tokens, cost, tool usage, and wall time per run. Idempotent:
existing result files are skipped, so interrupted runs resume cleanly.

Usage:
  bench.py run --tier 0 --out results/tier0
  bench.py run --repos chi --models haiku45 --variants base,lci \
      --difficulties easy --reps 1 --out results/smoke
"""

import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchlib as bl

PROMPT_TEMPLATE = (
    "Answer the following question about the codebase in the current "
    "directory. Explore the code as needed. Cite the relevant file paths "
    "(and line numbers where useful) and keep the answer concise.\n\n"
    "Question: {question}"
)

AGENTS_MD_LCI = """\
# Code exploration

This repository is indexed by Lightning Code Index (LCI). Prefer the `lci_*`
MCP tools over grep/find/cat for code questions:

- `lci_search` — semantic + content search for symbols and code
- `lci_get_context` — call hierarchy, callers/callees, dependencies of a symbol
- `lci_list_symbols` / `lci_inspect_symbol` — symbol lookup and details
- `lci_browse_file` — outline of a file's symbols
- `lci_find_files` — locate files by name/pattern
- `lci_semantic_annotations` — query project/domain annotations when present

Fall back to bash/read only when the LCI tools cannot answer.
"""


# lci-slim keeps the 5 navigation tools and drops the 9 auxiliary ones
# (~4.3k -> ~1.8k schema tokens per turn, fewer distracting choices).
SLIM_DISABLED = [
    "code_insight", "context", "debug_info", "git_analysis", "index_stats",
    "info", "inspect_symbol", "semantic_annotations", "side_effects",
]


def workspace_config(variant, lci_bin):
    cfg = {
        "$schema": "https://opencode.ai/config.json",
        "mcp": {"slop-mcp": {"enabled": False}},
        "permission": {"edit": "deny", "webfetch": "deny"},
    }
    if variant in ("lci", "lci-slim", "lci-ann"):
        cfg["mcp"]["lci"] = {"type": "local", "command": [lci_bin, "mcp"], "enabled": True}
    if variant == "lci-slim":
        cfg["tools"] = {f"lci_{name}": False for name in SLIM_DISABLED}
    return cfg


def install_annotation_overlay(repo, ws):
    src = os.path.join(bl.BENCH_ROOT, "annotations", repo)
    if not os.path.isdir(src):
        return
    dst = os.path.join(ws, ".lci", "annotations")
    if os.path.exists(dst):
        shutil.rmtree(dst)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copytree(src, dst)


def ensure_workspace(cfg, repo, variant):
    """Copy corpus to .work/<repo>-<variant>/ and drop variant config."""
    src = os.path.join(cfg.defaults["corpus-root"], cfg.repos[repo]["path"])
    ws = os.path.join(bl.WORK_DIR, f"{repo}-{variant}")
    stamp = os.path.join(ws, ".bench-ready")
    if not os.path.exists(stamp):
        if os.path.exists(ws):
            shutil.rmtree(ws)
        shutil.copytree(src, ws, symlinks=True)
        with open(stamp, "w") as f:
            f.write("ok\n")
    with open(os.path.join(ws, "opencode.json"), "w") as f:
        json.dump(workspace_config(variant, cfg.defaults["lci-bin"]), f, indent=2)
    agents = os.path.join(ws, "AGENTS.md")
    if variant == "lci-ann":
        install_annotation_overlay(repo, ws)
    if variant in ("lci", "lci-slim", "lci-ann"):
        with open(agents, "w") as f:
            f.write(AGENTS_MD_LCI)
    elif os.path.exists(agents):
        os.unlink(agents)
    return ws


def parse_events(lines):
    tools = []
    tokens = {"input": 0, "output": 0, "reasoning": 0, "cache_read": 0, "cache_write": 0}
    cost = 0.0
    steps = 0
    tool_output_chars = 0
    trace = []
    texts = {}       # messageID -> [text]
    order = []       # messageIDs in first-seen order
    error = None
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            e = json.loads(line)
        except json.JSONDecodeError:
            continue
        t = e.get("type")
        part = e.get("part", {})
        if t == "tool_use":
            tool = part.get("tool", "?")
            tools.append(tool)
            state = part.get("state", {})
            out = state.get("output")
            if isinstance(out, str):
                tool_output_chars += len(out)
            entry = {
                "tool": tool,
                "status": state.get("status", "?"),
                "output_chars": len(out) if isinstance(out, str) else 0,
            }
            args = state.get("input")
            if isinstance(args, dict):
                entry["args"] = {k: (v if isinstance(v, (int, float, bool)) else str(v)[:120])
                                 for k, v in args.items()}
            if state.get("status") == "error" or "error" in str(state.get("status", "")):
                entry["error"] = str(state.get("error") or out or "")[:300]
            # MCP tools report success at transport level but can carry a
            # JSON error payload in the output — surface those too.
            elif isinstance(out, str) and tool.startswith("lci_") and (
                    '"success": false' in out or '"error"' in out[:200]):
                entry["tool_error"] = out[:300]
            trace.append(entry)
        elif t == "step_finish":
            steps += 1
            tok = part.get("tokens")
            if tok:
                tokens["input"] += tok.get("input", 0)
                tokens["output"] += tok.get("output", 0)
                tokens["reasoning"] += tok.get("reasoning", 0)
                tokens["cache_read"] += tok.get("cache", {}).get("read", 0)
                tokens["cache_write"] += tok.get("cache", {}).get("write", 0)
            cost += part.get("cost", 0) or 0
        elif t == "text":
            mid = part.get("messageID", "?")
            if mid not in texts:
                texts[mid] = []
                order.append(mid)
            texts[mid].append(part.get("text", ""))
        elif t == "error":
            error = e.get("error")
    answer = "\n".join(texts[order[-1]]) if order else ""
    return {
        "answer": answer,
        "tokens": tokens,
        "cost": round(cost, 6),
        "steps": steps,
        "tool_calls": tools,
        "tool_output_chars": tool_output_chars,
        "tool_trace": trace,
        "error": error,
    }


def run_one(cfg, ws, model_id, question, timeout):
    prompt = PROMPT_TEMPLATE.format(question=question["question"])
    cmd = [cfg.defaults["opencode-bin"], "run", "--format", "json", "-m", model_id, prompt]
    start = time.time()
    try:
        proc = subprocess.run(
            cmd, cwd=ws, capture_output=True, text=True, timeout=timeout,
        )
        status = "ok" if proc.returncode == 0 else f"exit_{proc.returncode}"
        lines = proc.stdout.splitlines()
    except subprocess.TimeoutExpired as ex:
        status = "timeout"
        lines = (ex.stdout or "").splitlines() if isinstance(ex.stdout, str) else []
    wall = round(time.time() - start, 2)
    parsed = parse_events(lines)
    if status == "ok" and not parsed["answer"]:
        status = "empty_answer"
    if parsed["error"]:
        status = "provider_error"
    parsed.update({"status": status, "wall_seconds": wall})
    return parsed


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--tier")
    r.add_argument("--repos")
    r.add_argument("--models")
    r.add_argument("--variants", default="base,lci")
    r.add_argument("--difficulties")
    r.add_argument("--questions", help="comma-separated question ids to restrict to (smoke slices)")
    r.add_argument("--reps", type=int)
    r.add_argument("--out", required=True, help="results directory (relative to repo-qa/)")
    r.add_argument("--concurrency", type=int)
    r.add_argument("--retries", type=int, default=1)
    args = ap.parse_args()

    cfg = bl.Config()
    if args.tier:
        t = cfg.tier(args.tier)
    else:
        t = {"repos": [], "models": [], "difficulties": ["easy", "medium", "hard"], "reps": 1}
    repos = args.repos.split(",") if args.repos else t["repos"]
    models = args.models.split(",") if args.models else t["models"]
    difficulties = args.difficulties.split(",") if args.difficulties else t["difficulties"]
    only_qids = set(args.questions.split(",")) if args.questions else None
    reps = args.reps or t["reps"]
    variants = args.variants.split(",")
    if not repos or not models:
        ap.error("need --tier or explicit --repos/--models")
    for m in models:
        if m not in cfg.models:
            ap.error(f"unknown model alias {m}")
    out_dir = args.out if os.path.isabs(args.out) else os.path.join(bl.BENCH_ROOT, args.out)
    os.makedirs(out_dir, exist_ok=True)
    concurrency = args.concurrency or int(cfg.defaults["concurrency"])
    timeout = int(cfg.defaults["timeout-seconds"])

    jobs = []
    for repo in repos:
        questions = [q for q in bl.load_questions(repo)
                     if q["difficulty"] in difficulties
                     and (only_qids is None or q["id"] in only_qids)]
        for variant in variants:
            ws = ensure_workspace(cfg, repo, variant)
            for model in models:
                for q in questions:
                    for rep in range(1, reps + 1):
                        name = bl.result_name(repo, model, variant, q["id"], rep)
                        path = os.path.join(out_dir, name)
                        if os.path.exists(path):
                            continue
                        jobs.append((repo, variant, ws, model, q, rep, path))

    if not jobs:
        ap.error(
            "no runs to execute; check repo/model/difficulty/question filters "
            "and whether the output directory already contains all results"
        )

    # Schedule biggest-repo lci cells first, while provider quota is fresh.
    # Every tier-3 DNF was pocketbase-lci (the largest repo) hitting the go-plan
    # throttle at the session tail. Ordering the queue by repo size descending,
    # lci before base, moves that expensive grid to the front so the throttle
    # (which arrives late) lands on cheap, already-completed cells instead of
    # the one costly grid. Deterministic: ties break by model order, then
    # question id, then rep.
    repo_loc = {r: int(cfg.repos[r].get("loc", 0) or 0) for r in repos}
    variant_rank = {"lci-ann": 0, "lci": 1, "lci-slim": 2, "base": 3}
    model_pos = {m: i for i, m in enumerate(models)}
    jobs.sort(key=lambda j: (
        -repo_loc[j[0]],               # j[0]=repo   — biggest repo first
        variant_rank.get(j[1], 9),     # j[1]=variant — lci before base
        model_pos[j[3]],               # j[3]=model  — stable per-model grouping
        j[4]["id"], j[5],              # j[4]=question, j[5]=rep
    ))

    print(f"{len(jobs)} runs to execute (concurrency={concurrency})")

    def execute(job):
        repo, variant, ws, model, q, rep, path = job
        model_id = cfg.models[model]["id"]
        for attempt in range(args.retries + 1):
            res = run_one(cfg, ws, model_id, q, timeout)
            if res["status"] == "ok":
                break
        res.update({
            "repo": repo, "variant": variant, "model": model, "model_id": model_id,
            "question_id": q["id"], "difficulty": q["difficulty"],
            "question": q["question"], "rep": rep, "attempts": attempt + 1,
        })
        bl.write_json_atomic(path, res)
        tok = res["tokens"]
        print(f"[{res['status']:>14}] {os.path.basename(path)} "
              f"wall={res['wall_seconds']}s in={tok['input']} out={tok['output']} "
              f"cache={tok['cache_read']} tools={len(res['tool_calls'])}")
        return res["status"]

    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
        statuses = list(pool.map(execute, jobs))

    bad = [s for s in statuses if s != "ok"]
    print(f"done: {len(statuses) - len(bad)} ok, {len(bad)} failed")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
