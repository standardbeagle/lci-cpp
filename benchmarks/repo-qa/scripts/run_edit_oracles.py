#!/usr/bin/env python3
"""Run the stage-3 behaviour oracle for ONE edit task and emit its outcome.

Loads the task JSON, delegates to oracle_gate.evaluate_task_in_corpus (which
locates the forged corpus, loads the sidecar oracle patch named by
task.oracle_patch.path, resolves {edits_root}/{python} argv tokens, and
defaults the existing-suite command from task.existing_suite.command), then
prints the versioned oracle_gate_v1 outcome.

Exit status:

  * without --discrimination-only: 0 only when outcome.passed (all four
    sub-gates). NOTE: api_impact is API_IMPACT_NOT_EVALUATED by design until
    impacted symbols are derivable, so a full pass is unreachable today.
  * with --discrimination-only: 0 when discrimination.reason == DISCRIMINATES
    AND existing_suite.passed -- the two sub-gates the answer-key authors
    (M2-M4) own. Any other verdict exits 1.
"""

import argparse
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ORACLES = os.path.join(os.path.dirname(_HERE), "edits", "oracles")
for _path in (_HERE, _ORACLES):
    if _path not in sys.path:
        sys.path.insert(0, _path)

import oracle_gate as gate  # noqa: E402
import validate_edit_tasks as vedt  # noqa: E402  (DEFAULT_TASKS_DIR et al.)

DEFAULT_CORPUS_ROOT = os.path.join(
    vedt.BENCH_ROOT, ".work", "exploration"
)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--task-id", required=True, help="edit task id")
    parser.add_argument("--tasks-dir", default=vedt.DEFAULT_TASKS_DIR)
    parser.add_argument("--corpus-root", default=DEFAULT_CORPUS_ROOT)
    parser.add_argument("--timeout", type=int, default=gate.DEFAULT_TIMEOUT)
    parser.add_argument(
        "--evidence-dir",
        default=None,
        help="write the outcome to <evidence-dir>/<task-id>.oracle.json",
    )
    parser.add_argument(
        "--discrimination-only",
        action="store_true",
        help="exit 0 when the probe DISCRIMINATES and the existing suite is "
        "green (the two sub-gates answer-key authors own), ignoring the "
        "not-yet-evaluated api_impact gate",
    )
    args = parser.parse_args(argv)

    task_path = os.path.join(args.tasks_dir, f"{args.task_id}.json")
    if not os.path.isfile(task_path):
        print(f"error: no task file for id {args.task_id!r}: {task_path}",
              file=sys.stderr)
        return 2
    with open(task_path, encoding="utf-8") as handle:
        task = json.load(handle)
    if task.get("id") != args.task_id:
        print(
            f"error: task file {task_path} id {task.get('id')!r} != "
            f"{args.task_id!r}",
            file=sys.stderr,
        )
        return 2

    outcome = gate.evaluate_task_in_corpus(
        task, args.corpus_root, timeout=args.timeout
    )
    rendered = gate.to_json(outcome)
    print(rendered)

    if args.evidence_dir is not None:
        os.makedirs(args.evidence_dir, exist_ok=True)
        evidence_path = os.path.join(
            args.evidence_dir, f"{args.task_id}.oracle.json"
        )
        with open(evidence_path, "w", encoding="utf-8") as handle:
            handle.write(rendered + "\n")

    if args.discrimination_only:
        ok = (
            outcome["discrimination"]["reason"] == gate.Reason.DISCRIMINATES
            and outcome["existing_suite"]["passed"]
        )
    else:
        ok = outcome["passed"]
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
