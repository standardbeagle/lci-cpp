#!/usr/bin/env python3
"""Materialise the stage-2 claim-validation baseline's committed artifacts.

This is the stage-2 analogue of ``edit_baseline_ledger.py``: a deterministic,
provider-free generator that turns a pinned run-config + the committed task
bank into ``run-records.jsonl`` -> ``scores.json`` -> ``aggregate.json``.

The headline denominator is the PLANNED grid (task x arm x seed x rep) read
from the config, so with no paid run approved every cell is emitted with the
machine-readable ``not_executed`` status. Re-running the scorer over that
ledger yields a *correctly-empty* comparison (zero paired cells, empty deltas,
no arm's rate inflated by an absent divisor) rather than a fabricated answer --
the whole point being that an un-run cell can never masquerade as a zero-scored
attempt and leave the divisor.

    python3 benchmarks/repo-qa/scripts/claim_baseline_ledger.py \
        --config  benchmarks/repo-qa/exploration/run-configs/stage2-baseline.json \
        --tasks-dir benchmarks/repo-qa/exploration/tasks \
        --records benchmarks/repo-qa/results/exploration-stage2/run-records.jsonl \
        --out-dir benchmarks/repo-qa/results/exploration-stage2
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH_ROOT = os.path.dirname(HERE)
EXPLORATION_ROOT = os.path.join(BENCH_ROOT, "exploration")
for _p in (EXPLORATION_ROOT, HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import score_claim_validation as score_cli  # noqa: E402
from runner import record as record_log  # noqa: E402
from runner import toolsets  # noqa: E402
from runner.run import CLAIM_VALIDATION_MODE, CLAIM_VALIDATION_SCHEMA, _claim_digest  # noqa: E402
from scoring import CLAIM_SCORE_SET_SCHEMA  # noqa: E402

ARMS = (toolsets.TREATMENT, toolsets.BASELINE)
NOT_EXECUTED_STATUS = "not_executed"
NOT_EXECUTED_ERROR = "not_executed:paid_run_approval_not_granted"


def _load_json(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def _load_config(path):
    return json.loads(_load_json(path))


def _load_tasks(tasks_dir):
    return {
        task["id"]: task
        for task in (
            json.loads(_load_json(os.path.join(tasks_dir, name)))
            for name in sorted(os.listdir(tasks_dir))
            if name.endswith(".json")
        )
    }


def _planned_records(config, tasks):
    """One ``not_executed`` record per (task, arm, seed, rep), in run_key order.

    Every field the scorer's compatibility + digest checks require is set, so
    the emitted ledger is directly scoreable by ``score_claim_validation.py``
    with no provider call. The grid is built from the committed task bank, never
    truncated -- an omitted cell is forbidden by the S2.5 acceptance criteria.
    """
    model = config["model"]["intended"]
    settings = {
        "timeout_seconds": config["budget"]["proposed_time_budget_seconds_per_cell"],
        "system_prompt": config["provenance"]["harness"]["system_prompt"],
    }
    reps = config["repetitions"]["per_cell"]
    records = []
    for task_id in sorted(tasks):
        task = tasks[task_id]
        ref = task["manifest_ref"]
        seed = ref["seed"]
        for arm in ARMS:
            for rep in range(reps):
                records.append({
                    "run_key": record_log.run_key(
                        task_id, arm, seed, CLAIM_VALIDATION_MODE,
                        CLAIM_VALIDATION_SCHEMA),
                    "config_id": config["config_id"],
                    "task_id": task_id,
                    "corpus_id": ref["corpus_id"],
                    "source_commit": ref["source_commit"],
                    "forge_version": ref["forge_version"],
                    "seed": seed,
                    "rep": rep,
                    "arm": arm,
                    "model": model,
                    "mode": CLAIM_VALIDATION_MODE,
                    "schema_version": CLAIM_VALIDATION_SCHEMA,
                    "claim_task_digest": _claim_digest(task, CLAIM_VALIDATION_SCHEMA),
                    "settings": settings,
                    "effective_allowlist": list(toolsets.arm_allowlist(arm)),
                    "sealed_metadata": {
                        "task_id": task_id,
                        "category": task["author"]["category"],
                    },
                    "status": NOT_EXECUTED_STATUS,
                    "execution_state": "planned_not_executed",
                    "error": NOT_EXECUTED_ERROR,
                    "final_answer": "",
                    "tool_calls": [],
                    "token_usage": None,
                    "duration_seconds": None,
                    "manifest_id": None,
                    "violations": [],
                })
    return sorted(records, key=lambda rec: rec["run_key"])


def build(config_path, tasks_dir, records_path, out_dir):
    config = _load_config(config_path)
    tasks = _load_tasks(tasks_dir)
    records = _planned_records(config, tasks)

    os.makedirs(out_dir, exist_ok=True)
    tmp = records_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as handle:
        for rec in records:
            handle.write(json.dumps(rec, sort_keys=True) + "\n")
    os.replace(tmp, records_path)

    # Re-score the ledger we just wrote through the *exact* committed scorer, so
    # the published scores/aggregate are byte-identical to what CI re-derives.
    scored = score_cli.score_bank(tasks, records, settings=None)
    score_cli._write(os.path.join(out_dir, "scores.json"),
                     {"schema": CLAIM_SCORE_SET_SCHEMA,
                      "task_bank_digest": score_cli._bank_digest(tasks),
                      "scores": scored})
    aggregate = score_cli.aggregate_claim_scores(scored)
    score_cli._write(os.path.join(out_dir, "aggregate.json"), aggregate)

    comp = aggregate["pairing"]
    print(
        f"planned={len(records)} recorded={len(records)} executed=0 "
        f"paired={comp['paired_count']} unpaired={comp['unpaired_count']} "
        f"deltas={len(aggregate['deltas'])} -> {os.path.relpath(out_dir, BENCH_ROOT)}"
    )
    return aggregate


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--config",
        default=os.path.join(EXPLORATION_ROOT, "run-configs", "stage2-baseline.json"))
    parser.add_argument("--tasks-dir", default=os.path.join(EXPLORATION_ROOT, "tasks"))
    parser.add_argument(
        "--records",
        default=os.path.join(BENCH_ROOT, "results", "exploration-stage2",
                             "run-records.jsonl"))
    parser.add_argument(
        "--out-dir",
        default=os.path.join(BENCH_ROOT, "results", "exploration-stage2"))
    args = parser.parse_args(argv)
    build(args.config, args.tasks_dir, args.records, args.out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
