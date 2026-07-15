#!/usr/bin/env python3
"""Score a Stage-1 exploration run log against the adjudicated task bank.

Reads the S4 JSONL record log and the task answer keys, scores every run by
CITED EVIDENCE (see exploration/scoring), and emits two schema-versioned
artifacts under --out-dir:

  * scores.json    -- exploration_score_set_v1: one per-run score, sorted by
                      run key, each with evidence P/R/F1 and process metrics.
  * aggregate.json -- exploration_aggregate_v1: the paired-arm summary with
                      success rates, macro evidence, process distributions, and
                      LCI-minus-baseline deltas.

Fails loud (nonzero) on a record referencing an unknown task, and -- via
aggregate() -- on a run set that mixes models / forge versions / task-bank
versions unless --group-by names that field. It never invokes an LLM judge and
never reuses the repo-QA regex fact score; the primary result is cited-evidence
precision/recall, with tool calls / tokens / wall-clock as process signal.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "exploration"))

from runner import record as record_log  # noqa: E402
from scoring import AGGREGATE_SCHEMA, IncompatibleRuns, aggregate, score_run  # noqa: E402

SCORE_SET_SCHEMA = "exploration_score_set_v1"


def load_tasks(tasks_dir):
    """Map task id -> task object for every task file under tasks_dir."""
    if not os.path.isdir(tasks_dir):
        raise SystemExit(f"error: tasks dir does not exist: {tasks_dir}")
    tasks = {}
    for name in sorted(os.listdir(tasks_dir)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(tasks_dir, name), encoding="utf-8") as handle:
            task = json.load(handle)
        tasks[task["id"]] = task
    return tasks


def _write_json(path, payload):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
    os.replace(tmp, path)


def score_bank(tasks, records):
    """Score every record; fail loud on a record for an unknown task."""
    scores = []
    for rec in records:
        task_id = rec.get("task_id")
        task = tasks.get(task_id)
        if task is None:
            raise SystemExit(
                f"error: run record references unknown task_id {task_id!r} "
                f"(not in the task bank)"
            )
        scores.append(score_run(task, rec))
    scores.sort(key=lambda s: s["run_key"] or "")
    return scores


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tasks-dir", required=True)
    parser.add_argument("--records", required=True, help="S4 JSONL run log")
    parser.add_argument("--out-dir", required=True)
    parser.add_argument(
        "--group-by",
        default="",
        help="comma-separated compatibility fields to allow mixing "
        "(model,forge_version,task_bank_version)",
    )
    args = parser.parse_args(argv)

    tasks = load_tasks(args.tasks_dir)
    records = record_log.load_records(args.records)
    scores = score_bank(tasks, records)

    group_by = [field for field in args.group_by.split(",") if field]
    try:
        agg = aggregate(scores, group_by=group_by) if scores else {
            "schema": AGGREGATE_SCHEMA, "arms": {}, "deltas": {},
        }
    except IncompatibleRuns as error:
        raise SystemExit(f"error: {error}")

    _write_json(
        os.path.join(args.out_dir, "scores.json"),
        {"schema": SCORE_SET_SCHEMA, "scores": scores},
    )
    _write_json(os.path.join(args.out_dir, "aggregate.json"), agg)

    print(
        f"scored {len(scores)} run(s) across {len(agg.get('arms', {}))} arm(s) "
        f"-> {args.out_dir}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
