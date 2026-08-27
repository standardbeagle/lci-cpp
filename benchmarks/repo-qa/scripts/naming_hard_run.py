#!/usr/bin/env python3
"""Baseline (base-arm) run of the naming-hard task set with a cheap model.

For each question in questions/naming-hard-chi.json, runs `opencode run` in a
fresh git copy of the chi-base corpus with READ-ONLY tools enabled (grep /
glob / read / list; no edit, no bash, no web), records the answer text plus
usage tokens and tool-call count, and appends one resume-idempotent JSON
record per question to the --out file.

Grading is a separate step (hand or judge): a task is HARD for the model if
the answer is wrong OR total tokens > 2x the model's own median cost across
the control questions (class control-*). Pre-registered predictions:
exploration/naming-hard/predictions.md.

Cost posture: free-tier model by default; nothing here touches paid ids.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

import opencode_runner as runner
import replay_common

HERE = Path(__file__).resolve().parent
BENCH_ROOT = HERE.parent
DEFAULT_MODEL = "opencode/mimo-v2.5-free"  # weak tier; deepseek-v4-flash-free retired

PROMPT_TEMPLATE = """You are answering a question about the Go repository in the current directory (chi, an HTTP router). Use the available tools (grep, glob, read) to find the answer in the code. Answer precisely, citing file and line. Do not guess: if you cannot find it, say so.

Question: {question}
"""


def corpus_workspace(corpus: Path, parent: Path) -> Path:
    """A disposable git copy of the corpus with read-only tools allowed."""
    workspace = Path(tempfile.mkdtemp(prefix="naming-hard-", dir=parent))
    try:
        # Copy the working tree, not .git — the model gets code, no history.
        for entry in corpus.iterdir():
            if entry.name == ".git":
                continue
            dest = workspace / entry.name
            if entry.is_dir():
                shutil.copytree(entry, dest, symlinks=True)
            else:
                shutil.copy2(entry, dest)
        (workspace / "opencode.json").write_text(replay_common.pretty({
            "$schema": "https://opencode.ai/config.json",
            "permission": {
                "edit": "deny",
                "bash": "deny",
                "webfetch": "deny",
            },
        }))
        subprocess.run(["git", "init", "-q"], cwd=workspace, check=True)
        subprocess.run(["git", "add", "-A"], cwd=workspace, check=True)
        subprocess.run(
            ["git", "-c", "user.name=Harness", "-c", "user.email=harness@invalid",
             "commit", "-qm", "naming-hard workspace"],
            cwd=workspace, check=True,
        )
        return workspace
    except BaseException:
        shutil.rmtree(workspace, ignore_errors=True)
        raise


def load_done(out_path: Path) -> set[str]:
    done = set()
    if out_path.exists():
        for line in out_path.read_text().splitlines():
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if rec.get("status") == "answered":
                done.add((rec.get("id"), rec.get("model")))
    return done


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--questions",
                        default=str(BENCH_ROOT / "questions" / "naming-hard-chi.json"))
    parser.add_argument("--corpus",
                        default=str(BENCH_ROOT / ".work" / "chi-base"))
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--opencode", default="opencode")
    parser.add_argument("--timeout", type=int, default=420)
    parser.add_argument("--out",
                        default=str(BENCH_ROOT / "results" / "naming-hard" /
                                    "base-arm.jsonl"))
    parser.add_argument("--only", help="comma-separated question ids")
    args = parser.parse_args()

    if "/" not in args.model:
        raise SystemExit("--model must be a full provider/model id, not an alias")

    bank = json.loads(Path(args.questions).read_text())
    questions = bank["questions"]
    if args.only:
        wanted = set(args.only.split(","))
        questions = [q for q in questions if q["id"] in wanted]

    corpus = Path(args.corpus)
    if not corpus.is_dir():
        raise SystemExit(f"corpus not found: {corpus}")

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    done = load_done(out_path)

    scratch = Path(tempfile.mkdtemp(prefix="naming-hard-runs-"))
    failures = 0
    try:
        for q in questions:
            key = (q["id"], args.model)
            if key in done:
                print(json.dumps({"id": q["id"], "skip": "already done"}))
                continue
            workspace = corpus_workspace(corpus, scratch)
            started = time.time()
            try:
                run = runner.run_opencode(
                    args.opencode, workspace, args.model,
                    PROMPT_TEMPLATE.format(question=q["question"]),
                    args.timeout)
            finally:
                shutil.rmtree(workspace, ignore_errors=True)
            # Tool-call count: opencode emits one JSON event per tool part;
            # count them from the raw stream (parse_events keeps only text).
            tool_calls = 0
            for line in (run.get("raw_stdout") or "").splitlines():
                try:
                    ev = json.loads(line)
                except json.JSONDecodeError:
                    continue
                part = ev.get("part", ev) if isinstance(ev, dict) else {}
                if isinstance(part, dict) and str(part.get("type", "")).startswith("tool"):
                    tool_calls += 1
            record = {
                "id": q["id"],
                "class": q.get("class"),
                "model": args.model,
                "elapsed_s": round(time.time() - started, 1),
                "status": run.get("status"),
                "tokens": run.get("tokens"),
                "tool_calls": tool_calls,
                "wall_seconds": run.get("wall_seconds"),
                "answer": run.get("answer", ""),
                "provider_error": run.get("provider_error"),
            }
            if record["status"] != "answered":
                failures += 1
            with out_path.open("a") as fh:
                fh.write(json.dumps(record, sort_keys=True) + "\n")
            print(json.dumps({"id": q["id"], "status": record["status"],
                              "elapsed_s": record["elapsed_s"],
                              "tokens": record["tokens"],
                              "tool_calls": tool_calls}))
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    return 1 if failures == len(questions) and questions else 0


if __name__ == "__main__":
    raise SystemExit(main())
