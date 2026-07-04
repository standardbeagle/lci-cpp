#!/usr/bin/env python3
"""Score benchmark results: deterministic fact accuracy + LLM judge.

Adds a "scores" object to each result file (atomic rewrite). Idempotent:
results that already carry scores are skipped unless --rejudge.

Usage:
  judge.py --dir results/tier0
  judge.py --dir results/tier0 --skip-llm      # facts only, free
"""

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchlib as bl

JUDGE_PROMPT = """\
You are grading an AI coding assistant's answer to a question about a codebase.

Question:
{question}

Reference answer (ground truth, verified against the code):
{gold}

Candidate answer:
{answer}

Score the candidate 1-5 on each dimension:
- correctness: factual agreement with the reference (5 = no errors, 1 = mostly wrong)
- completeness: covers the key points of the reference (5 = all, 1 = almost none)
- grounding: cites the right files/symbols rather than vague or invented ones
- conciseness: signal density; penalize padding and irrelevant material

Respond with ONLY a JSON object, no markdown, no commentary:
{{"correctness": N, "completeness": N, "grounding": N, "conciseness": N, "rationale": "one sentence"}}
"""


def judge_workspace(cfg):
    ws = os.path.join(bl.WORK_DIR, "judge")
    os.makedirs(ws, exist_ok=True)
    with open(os.path.join(ws, "opencode.json"), "w") as f:
        json.dump({
            "$schema": "https://opencode.ai/config.json",
            "mcp": {"slop-mcp": {"enabled": False}},
            "permission": {"edit": "deny", "bash": "deny", "webfetch": "deny"},
        }, f)
    return ws


def llm_judge(cfg, ws, question, gold, answer, timeout=240):
    prompt = JUDGE_PROMPT.format(question=question, gold=gold, answer=answer or "(empty answer)")
    cmd = [cfg.defaults["opencode-bin"], "run", "-m", cfg.defaults["judge-model"], prompt]
    try:
        proc = subprocess.run(cmd, cwd=ws, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"error": f"judge timeout after {timeout}s"}
    start = proc.stdout.find("{")
    if start < 0:
        return {"error": f"no JSON in judge output (rc={proc.returncode})"}
    try:
        j, _ = json.JSONDecoder().raw_decode(proc.stdout[start:])
    except json.JSONDecodeError as ex:
        return {"error": f"judge JSON parse: {ex}"}
    dims = ("correctness", "completeness", "grounding", "conciseness")
    if not all(isinstance(j.get(d), (int, float)) and 1 <= j[d] <= 5 for d in dims):
        return {"error": f"judge scores out of range: {j}"}
    j["overall"] = round(sum(j[d] for d in dims) / len(dims), 2)
    return j


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--skip-llm", action="store_true")
    ap.add_argument("--rejudge", action="store_true")
    ap.add_argument("--concurrency", type=int, default=4)
    args = ap.parse_args()

    cfg = bl.Config()
    run_dir = args.dir if os.path.isabs(args.dir) else os.path.join(bl.BENCH_ROOT, args.dir)
    ws = judge_workspace(cfg)
    banks = {}

    todo = []
    n_skip = 0
    for path, res in bl.iter_results(run_dir):
        if "scores" in res and not args.rejudge:
            n_skip += 1
            continue
        repo = res["repo"]
        if repo not in banks:
            banks[repo] = {q["id"]: q for q in bl.load_questions(repo)}
        todo.append((path, res, banks[repo][res["question_id"]]))

    def score(item):
        path, res, q = item
        acc, hits = bl.fact_accuracy(res.get("answer", ""), q["must_mention"])
        scores = {"fact_accuracy": acc, "fact_hits": hits}
        if q.get("expected_tools"):
            used = set(res.get("tool_calls", []))
            scores["tool_match"] = int(any(t in used for t in q["expected_tools"]))
        if not args.skip_llm:
            if res.get("status") == "ok":
                scores["judge"] = llm_judge(cfg, ws, q["question"], q["gold_answer"], res["answer"])
            else:
                scores["judge"] = {"error": f"run status {res.get('status')}"}
        res["scores"] = scores
        bl.write_json_atomic(path, res)
        j = scores.get("judge", {})
        print(f"{os.path.basename(path)}: facts={acc:.2f} "
              f"judge={j.get('overall', '-')}"
              + (f" JUDGE_ERR={j['error']}" if "error" in j else ""))
        return "error" in j

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        errs = list(pool.map(score, todo))
    n_err = sum(errs)
    print(f"judged {len(todo)}, skipped {n_skip}, judge errors {n_err}")
    sys.exit(1 if n_err else 0)


if __name__ == "__main__":
    main()
