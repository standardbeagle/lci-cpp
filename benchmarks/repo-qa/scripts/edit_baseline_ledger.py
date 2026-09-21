"""Materialise the stage-3 edit baseline's committed score/aggregate from its
run-config + live record log.

This is the stage-3 analogue of ``score_exploration.py``: a deterministic
generator that turns (a pinned run-config + a record log) into ``scores.json``
and ``aggregate.json``. Re-running it on the same inputs yields byte-identical
output (jsonschema-free: it only calls the scorer), so CI can diff the
committed artifacts against a fresh derivation and catch drift.

The headline denominator is the PLANNED grid (task x arm x seed) read from the
config, NOT the set of records that came back. So with an empty paid ledger the
aggregate correctly reports every planned cell as ``missing_cell`` and fails
completeness -- an un-run cell can never inflate a rate by leaving the divisor.

    python3 benchmarks/repo-qa/scripts/edit_baseline_ledger.py \
        --config  benchmarks/repo-qa/edits/run-config.json \
        --records benchmarks/repo-qa/edits/results/baseline/run-records.jsonl \
        --out-dir benchmarks/repo-qa/edits/results/baseline
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH_ROOT = os.path.dirname(HERE)
EDITS = os.path.join(BENCH_ROOT, "edits")
for _p in (
    os.path.join(EDITS, "runner"),
    os.path.join(EDITS, "scoring"),
    os.path.join(BENCH_ROOT, "exploration"),
    HERE,
):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import edit_paths  # noqa: E402,F401  (wires the shared runner + gates)
import edit_record  # noqa: E402
import edit_toolsets  # noqa: E402

import edit_scorer  # noqa: E402


def _load_config(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def _task_ids(tasks_dir):
    return sorted(
        json.load(open(os.path.join(tasks_dir, name), encoding="utf-8"))["id"]
        for name in os.listdir(tasks_dir)
        if name.endswith(".json")
    )


def _write_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(obj, handle, indent=2, sort_keys=True)
        handle.write("\n")
    os.replace(tmp, path)


def build(config_path, tasks_dir, records_path, out_dir):
    config = _load_config(config_path)
    task_ids = _task_ids(tasks_dir)
    # Canonical arm set: the runner's own definition, not the config's keys
    # (the config's "arms" object also carries a non-arm documentation note).
    arms = [edit_toolsets.TREATMENT, edit_toolsets.BASELINE]
    for arm in arms:
        if arm not in config["arms"]:
            raise SystemExit(f"config arms missing canonical arm {arm!r}")
    seeds = list(config["seeds"])

    planned = edit_scorer.planned_cells(task_ids, arms, seeds)

    records = edit_record.load_records(records_path) if os.path.isfile(records_path) else []
    scores = []
    for rec in records:
        score = edit_scorer.score_run(rec)
        edit_scorer.validate_score(score)
        scores.append(score)

    report = edit_scorer.aggregate(scores, planned)
    edit_scorer.validate_aggregate(report)

    os.makedirs(out_dir, exist_ok=True)
    _write_json(os.path.join(out_dir, "scores.json"), scores)
    _write_json(os.path.join(out_dir, "aggregate.json"), report)

    comp = report["completeness"]
    print(
        f"planned={comp['planned_cells']} observed={comp['observed_cells']} "
        f"missing={comp['missing_cells']} completeness_passed={comp['passed']} "
        f"-> {os.path.relpath(out_dir, BENCH_ROOT)}/aggregate.json"
    )
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--config", default=os.path.join(EDITS, "run-config.json")
    )
    parser.add_argument("--tasks-dir", default=os.path.join(EDITS, "tasks"))
    parser.add_argument(
        "--records",
        default=os.path.join(EDITS, "results", "baseline", "run-records.jsonl"),
    )
    parser.add_argument(
        "--out-dir", default=os.path.join(EDITS, "results", "baseline")
    )
    args = parser.parse_args(argv)
    build(args.config, args.tasks_dir, args.records, args.out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
