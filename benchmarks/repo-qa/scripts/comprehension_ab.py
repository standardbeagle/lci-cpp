#!/usr/bin/env python3
"""Run and grade canned-output comprehension cells across LCI's tool surface."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay_common


ROOT = Path(__file__).resolve().parents[3]
SURFACE = ROOT / "benchmarks/repo-qa/comprehension/surface/tool-surface.json"
VARIANTS = ROOT / "benchmarks/repo-qa/comprehension/formats/variants.json"
DEFAULT_MODELS = ["opencode/deepseek-v4-flash-free", "opencode-go/glm-5.2"]
MODEL_TIERS = {
    "opencode/deepseek-v4-flash-free": "weak",
    "opencode-go/glm-5.2": "strong",
}
PROMPT = """Answer the QUESTION using ONLY the TOOL OUTPUT below. Do not use tools or outside knowledge.
Return strict JSON only: {{\"answers\":[\"one atomic answer\",\"another\"]}}.
Include every supported answer and nothing unsupported. Preserve file:line locations exactly.

QUESTION:
{question}

TOOL OUTPUT:
{blob}
"""


canonical = replay_common.pretty  # byte-identical to the previous local copy


def parse_events(lines: list[str]) -> tuple[str, dict]:
    texts: dict[str, list[str]] = {}
    order: list[str] = []
    usage = {"input": 0, "output": 0, "reasoning": 0}
    provider_error = None
    malformed_event = False
    for line in lines:
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(event, dict):
            continue
        part = event.get("part", event)
        if not isinstance(part, dict):
            continue
        kind = part.get("type")
        if kind == "text":
            message = part.get("messageID", "?")
            text = part.get("text", "")
            if not isinstance(message, str) or not isinstance(text, str):
                malformed_event = True
                continue
            if message not in texts:
                texts[message] = []
                order.append(message)
            texts[message].append(text)
        elif kind == "step_finish":
            tokens = part.get("tokens") or {}
            if not isinstance(tokens, dict):
                tokens = {}
            for key in usage:
                value = tokens.get(key, 0) or 0
                if not isinstance(value, (int, float)) or isinstance(value, bool):
                    malformed_event = True
                    continue
                usage[key] += value
        elif kind == "error":
            provider_error = event.get("error") or part.get("error") or "provider error"
    return ("\n".join(texts[order[-1]]) if order else ""), {
        "tokens": usage, "provider_error": provider_error,
        "malformed_event": malformed_event,
    }


def parse_answers(answer: str) -> list[str]:
    candidate = answer.strip()
    fenced = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", candidate, re.DOTALL)
    if fenced:
        candidate = fenced.group(1)
    value = json.loads(candidate)
    if not isinstance(value, dict):
        raise ValueError("answer JSON root must be an object")
    answers = value.get("answers")
    if not isinstance(answers, list) or any(not isinstance(item, str) for item in answers):
        raise ValueError("answer must be a JSON object with string answers[]")
    return answers


normalize_answer = replay_common.normalize_term


def grade(extracted: list[str], expected: list[str]) -> dict:
    scored = replay_common.grade_sets({normalize_answer(item) for item in extracted},
                                      {normalize_answer(item) for item in expected})
    return {
        "precision": scored["precision"],
        "recall": scored["recall"],
        "false_positive_count": len(scored["false_positives"]),
        "false_positives": scored["false_positives"],
        "false_negatives": scored["false_negatives"],
        "exact": scored["exact"],
    }


def empty_git_workspace(parent: Path) -> Path:
    workspace = Path(tempfile.mkdtemp(prefix="comprehension-empty-", dir=parent))
    try:
        subprocess.run(["git", "init", "-q"], cwd=workspace, check=True)
        (workspace / ".gitignore").write_text("*\n!.gitignore\n!opencode.json\n")
        (workspace / "opencode.json").write_text(canonical({
            "$schema": "https://opencode.ai/config.json",
            "mcp": {},
            "tools": {"*": False},
            "permission": {"*": "deny"},
        }))
        subprocess.run(["git", "add", ".gitignore", "opencode.json"], cwd=workspace, check=True)
        subprocess.run(
            ["git", "-c", "user.name=Comprehension Harness", "-c", "user.email=harness@invalid", "commit", "-qm", "empty harness workspace"],
            cwd=workspace, check=True,
        )
        return workspace
    except BaseException:
        shutil.rmtree(workspace)
        raise


def cell_key(tool: str, variant: str, model: str, repetition: int) -> str:
    model_key = re.sub(r"[^A-Za-z0-9_.-]+", "_", model)
    model_digest = hashlib.sha256(model.encode()).hexdigest()[:10]
    return f"{tool}__{variant}__{model_key}-{model_digest}__r{repetition}"


def parse_model_list(value: str) -> list[str]:
    """Split --models, tolerating spaces so ids still key MODEL_TIERS exactly."""
    models = [model.strip() for model in value.split(",")]
    models = [model for model in models if model]
    if not models:
        raise ValueError("--models must name at least one model")
    if any("/" not in model for model in models):
        raise ValueError("models must be full provider/model ids, not aliases")
    return models


def classify_failure(message: str) -> str | None:
    lowered = message.casefold()
    if any(marker in lowered for marker in ("quota", "rate limit", "rate_limit", "too many requests", "429")):
        return "provider_quota"
    return None


def run_model(opencode: str, workspace: Path, model: str, prompt: str, timeout: int) -> dict:
    started = time.monotonic()
    try:
        environment = os.environ.copy()
        environment["OPENCODE_CONFIG"] = str(workspace / "opencode.json")
        environment["XDG_CONFIG_HOME"] = str(workspace / ".xdg-config")
        proc = subprocess.run(
            [opencode, "run", "--format", "json", "-m", model, prompt],
            cwd=workspace, env=environment, text=True, capture_output=True, timeout=timeout,
        )
        raw_answer, metadata = parse_events(proc.stdout.splitlines())
        failure_text = json.dumps(metadata["provider_error"]) + "\n" + proc.stderr
        classified = classify_failure(failure_text)
        if metadata["malformed_event"]:
            status = "malformed_provider_stream"
        elif classified:
            status = classified
        elif metadata["provider_error"]:
            status = "provider_error"
        elif proc.returncode == 124:
            status = "provider_timeout"
        elif proc.returncode != 0:
            status = f"exit_{proc.returncode}"
        elif not raw_answer.strip():
            status = "empty_answer"
        else:
            status = "answered"
    except subprocess.TimeoutExpired as exc:
        raw = exc.stdout if isinstance(exc.stdout, str) else ""
        raw_answer, metadata = parse_events(raw.splitlines())
        status = "provider_timeout"
    return {
        "status": status, "answer": raw_answer,
        "wall_seconds": round(time.monotonic() - started, 3), **metadata,
    }


def write_atomic(path: Path, value: dict) -> None:
    replay_common.write_atomic(path, canonical(value))


def load_bank() -> list[dict]:
    surface = json.loads(SURFACE.read_text())
    variants = json.loads(VARIANTS.read_text())
    questions = {item["name"]: item["question"] for item in surface["tools"]}
    cells = []
    for tool in variants["tools"]:
        for variant in tool["variants"]:
            cells.append({
                "tool": tool["name"], "variant": variant["id"],
                "question": questions[tool["name"]], "blob": variant["text"],
                "expected": tool["answer"],
                "production_faithful": variant["production_faithful"],
            })
    return cells


def prompt_digest(prompt: str) -> str:
    return hashlib.sha256(prompt.encode()).hexdigest()


def reusable_result(path: Path, identity: dict) -> bool:
    if not path.exists():
        return False
    try:
        existing = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"corrupt resume result {path}: {exc}") from exc
    for key, value in identity.items():
        if existing.get(key) != value:
            return False
    status = existing.get("status")
    if status == "answered":
        score = existing.get("score")
        return (
            isinstance(existing.get("answer"), str)
            and isinstance(existing.get("extracted"), list)
            and all(isinstance(item, str) for item in existing["extracted"])
            and isinstance(score, dict)
            and {"precision", "recall", "false_positive_count", "false_positives", "false_negatives", "exact"} <= set(score)
        )
    if status == "malformed_answer":
        return (
            isinstance(existing.get("answer"), str)
            and existing.get("score") is None
            and isinstance(existing.get("parse_error"), str)
        )
    return False


def execute_cell(opencode: str, workspace: Path, cell: dict, model: str,
                 repetition: int, timeout: int, path: Path) -> dict:
    prompt = PROMPT.format(question=cell["question"], blob=cell["blob"])
    identity = {
        "schema": "lci.comprehension.cell.v1",
        "cell_key": cell_key(cell["tool"], cell["variant"], model, repetition),
        "tool": cell["tool"], "variant": cell["variant"], "model": model,
        "capability_tier": MODEL_TIERS.get(model, "unclassified"),
        "repetition": repetition, "prompt_digest": prompt_digest(prompt),
        "input_digest": hashlib.sha256(canonical({
            "grading_schema": "precision-recall-fp.v1",
            "question": cell["question"], "blob": cell["blob"],
            "expected": cell["expected"],
            "production_faithful": cell["production_faithful"],
        }).encode()).hexdigest(),
    }
    if reusable_result(path, identity):
        return json.loads(path.read_text())
    run = run_model(opencode, workspace, model, prompt, timeout)
    result = {**identity, "production_faithful": cell["production_faithful"], **run}
    if run["status"] == "answered":
        try:
            extracted = parse_answers(run["answer"])
            result["extracted"] = extracted
            result["score"] = grade(extracted, cell["expected"])
        except (json.JSONDecodeError, ValueError, TypeError, AttributeError) as exc:
            result["status"] = "malformed_answer"
            result["score"] = None
            result["parse_error"] = str(exc)
    else:
        result["score"] = None
    write_atomic(path, result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--models", default=",".join(DEFAULT_MODELS))
    parser.add_argument("--reps", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--opencode", default="opencode")
    parser.add_argument("--tool", action="append", help="restrict to a tool (repeatable)")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    try:
        models = parse_model_list(args.models)
    except ValueError as error:
        parser.error(str(error))
    if args.timeout < 300:
        parser.error("--timeout must be at least 300 seconds")
    cells = [cell for cell in load_bank() if not args.tool or cell["tool"] in args.tool]
    jobs = [(cell, model, rep) for cell in cells for model in models for rep in range(1, args.reps + 1)]
    if args.dry_run:
        print(json.dumps({"cells": len(jobs), "models": models}, sort_keys=True))
        return 0
    args.out.mkdir(parents=True, exist_ok=True)
    workspace = empty_git_workspace(args.out)
    try:
        for cell, model, repetition in jobs:
            key = cell_key(cell["tool"], cell["variant"], model, repetition)
            path = args.out / f"{key}.json"
            execute_cell(args.opencode, workspace, cell, model, repetition, args.timeout, path)
    finally:
        shutil.rmtree(workspace)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
