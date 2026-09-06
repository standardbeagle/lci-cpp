#!/usr/bin/env python3
"""Run the stage-3 behaviour oracle for ONE edit task and emit its outcome.

With --all, sweep every task in --tasks-dir instead: one
'<id> <discrimination.reason> <existing_suite.passed>' line per task,
exit 1 if any is not DISCRIMINATES.

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


def _evaluate_one(args, task_id):
    """Evaluate one task and return (outcome, error_exit_code_or_None)."""
    task_path = os.path.join(args.tasks_dir, f"{task_id}.json")
    if not os.path.isfile(task_path):
        print(f"error: no task file for id {task_id!r}: {task_path}",
              file=sys.stderr)
        return None, 2
    with open(task_path, encoding="utf-8") as handle:
        task = json.load(handle)
    if task.get("id") != task_id:
        print(
            f"error: task file {task_path} id {task.get('id')!r} != "
            f"{task_id!r}",
            file=sys.stderr,
        )
        return None, 2
    return gate.evaluate_task_in_corpus(
        task, args.corpus_root, timeout=args.timeout
    ), None


def _run_all(args):
    """Sweep every task in the tasks dir, one line per id.

    Prints '<id> <discrimination.reason> <existing_suite.passed>' per task
    in sorted id order and exits 1 if any task is not DISCRIMINATES (with
    --discrimination-only a red existing suite also fails the sweep).
    """
    if args.evidence_dir is not None:
        print("error: --evidence-dir is not supported with --all",
              file=sys.stderr)
        return 2
    task_ids = sorted(
        name[: -len(".json")]
        for name in os.listdir(args.tasks_dir)
        if name.endswith(".json")
    )
    if not task_ids:
        print(f"error: no task files in {args.tasks_dir}", file=sys.stderr)
        return 2
    all_ok = True
    for task_id in task_ids:
        outcome, err = _evaluate_one(args, task_id)
        if err is not None:
            return err
        reason = outcome["discrimination"]["reason"]
        suite_passed = outcome["existing_suite"]["passed"]
        print(f"{task_id} {reason} {suite_passed}")
        if reason != gate.Reason.DISCRIMINATES or (
            args.discrimination_only and not suite_passed
        ):
            all_ok = False
    return 0 if all_ok else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--task-id", default=None, help="edit task id")
    parser.add_argument(
        "--all",
        action="store_true",
        help="sweep every task in --tasks-dir: one '<id> <reason> "
        "<suite-passed>' line each, exit 1 if any is not DISCRIMINATES",
    )
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

    if args.all:
        if args.task_id is not None:
            print("error: --all and --task-id are mutually exclusive",
                  file=sys.stderr)
            return 2
        return _run_all(args)
    if args.task_id is None:
        print("error: one of --task-id or --all is required",
              file=sys.stderr)
        return 2

    outcome, err = _evaluate_one(args, args.task_id)
    if err is not None:
        return err
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
