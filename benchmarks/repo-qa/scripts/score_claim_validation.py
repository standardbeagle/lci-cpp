#!/usr/bin/env python3
"""Score isolated claim-validation JSONL records without an LLM judge."""

import argparse
import hashlib
import json
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "exploration")
sys.path.insert(0, ROOT)

from runner import record as record_log  # noqa: E402
from runner.run import CLAIM_VALIDATION_MODE, _claim_digest  # noqa: E402
from scoring import (CLAIM_SCORE_SET_SCHEMA, IncompatibleRuns,
                     aggregate_claim_scores, score_claim_run)  # noqa: E402


def _load_tasks(path):
    tasks = {}
    for name in sorted(os.listdir(path)):
        if name.endswith(".json"):
            with open(os.path.join(path, name), encoding="utf-8") as handle:
                task = json.load(handle)
            tasks[task["id"]] = task
    return tasks


def _bank_digest(tasks):
    payload = [(key, tasks[key]) for key in sorted(tasks)]
    raw = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    return "sha256:" + hashlib.sha256(raw).hexdigest()


def _write(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(value, handle, indent=2, sort_keys=True)
    os.replace(tmp, path)


def score_bank(tasks, records, settings=None):
    latest = {}
    for index, rec in enumerate(records):
        key = rec.get("run_key")
        if key is None:
            raise SystemExit(f"error: record {index} is missing run_key")
        latest[key] = rec
    digest = _bank_digest(tasks)
    scores = []
    for rec in latest.values():
        if rec.get("mode") != CLAIM_VALIDATION_MODE:
            raise SystemExit("error: non-claim-validation record in claim score input")
        task_id = (rec.get("sealed_metadata") or {}).get("task_id")
        task = tasks.get(task_id)
        if task is None:
            raise SystemExit(f"error: unknown sealed task_id {task_id!r}")
        if rec.get("claim_task_digest") != _claim_digest(task, rec.get("schema_version")):
            raise SystemExit(f"error: claim task digest mismatch for {task_id!r}")
        scores.append(score_claim_run(task, rec, task_bank_digest=digest, settings=settings))
    return sorted(scores, key=lambda score: score["run_key"])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tasks-dir", required=True)
    parser.add_argument("--records", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--settings-json", default="{}")
    args = parser.parse_args(argv)
    try:
        settings = json.loads(args.settings_json)
    except json.JSONDecodeError as error:
        raise SystemExit(f"error: invalid settings JSON: {error}")
    if not isinstance(settings, dict):
        raise SystemExit("error: settings JSON must be an object")
    tasks = _load_tasks(args.tasks_dir)
    scores = score_bank(tasks, record_log.load_records(args.records), settings)
    try:
        aggregate = aggregate_claim_scores(scores)
    except IncompatibleRuns as error:
        raise SystemExit(f"error: {error}")
    _write(os.path.join(args.out_dir, "scores.json"),
           {"schema": CLAIM_SCORE_SET_SCHEMA, "task_bank_digest": _bank_digest(tasks), "scores": scores})
    _write(os.path.join(args.out_dir, "aggregate.json"), aggregate)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
