#!/usr/bin/env python3
"""One command over the E2.3 selection grid: plan / run / report.

  plan    print every (task x variant x model) cell and its run key
  run     execute the cells missing from the records file, appending one
          record each; re-running is a no-op for cells already recorded
  report  confusion matrix per (variant, model), stratified by tier

The grading, workspace wiring and matrix aggregation live in
toolcalling/runner/; this file is only the CLI and the resume ledger.

Live runs cost provider calls, so `run` is opted in with --live (or
SELECTION_AB_LIVE=1) and skips loudly otherwise. Results land outside the
repo by default (benchmarks/repo-qa/.work/toolcalling/, gitignored).

Recipe inherited from the working opencode harnesses and not to be
re-derived: a GIT workspace (a bare directory hangs), the mock wired through
the opencode.json mcp block, FULL provider/model ids (never a config alias),
timeout >= 300s, and provider quota kills tagged distinctly from a genuine
wrong selection.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
BENCH_ROOT = HERE.parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import opencode_runner  # noqa: E402

MANIFEST = BENCH_ROOT / "comprehension/surface/tool-surface.json"
TASKS_DIR = BENCH_ROOT / "toolcalling/tasks"
DESCRIPTIONS_DIR = BENCH_ROOT / "toolcalling/descriptions"
DEFAULT_OUT = BENCH_ROOT / ".work/toolcalling"
DEFAULT_RECORDS = DEFAULT_OUT / "records.jsonl"

# Full provider/model ids; never a config alias, never a paid provider.
# `opencode/deepseek-v4-flash-free` is no longer served: `opencode models`
# (1.18.26, 2026-09-06) lists no deepseek under the `opencode` provider, and a
# cell against it returns UnknownError in 6s or hangs to the 300s timeout with
# an empty stream. The same model is served on the go plan, which is already
# the strong arm's route, so both arms cost the same.
MODELS = {
    "weak": "opencode-go/deepseek-v4-flash",
    "strong": "opencode-go/glm-5.2",
}
DEFAULT_TIMEOUT = 300.0

SYSTEM_PREFIX = (
    "You have a set of code-intelligence tools available. Choose the single "
    "tool that best serves the request and call it. Request:\n\n"
)


def load_selection_runner():
    """Load toolcalling/runner as `selection_runner`.

    benchmarks/repo-qa already has an importable top-level `runner` package
    (exploration/runner), so putting toolcalling/ on sys.path would make
    `import runner` depend on which harness ran first.
    """
    if "selection_runner" in sys.modules:
        return sys.modules["selection_runner"]
    root = BENCH_ROOT / "toolcalling" / "runner"
    spec = importlib.util.spec_from_file_location(
        "selection_runner", root / "__init__.py",
        submodule_search_locations=[str(root)])
    module = importlib.util.module_from_spec(spec)
    sys.modules["selection_runner"] = module
    spec.loader.exec_module(module)
    return module


sr = load_selection_runner()


# --------------------------------------------------------------------- planning

def variant_path(variant: str) -> Path:
    matches = sorted(DESCRIPTIONS_DIR.glob(f"variant-{variant}-*.json"))
    if len(matches) != 1:
        raise SystemExit(f"variant {variant!r} does not name exactly one file: {matches}")
    return matches[0]


def load_tasks(tasks_dir: Path) -> list[dict]:
    return [json.loads(p.read_text()) for p in sorted(Path(tasks_dir).glob("*.json"))]


def plan_cells(tasks_dir: Path, variants, models) -> list[dict]:
    cells = []
    for task in load_tasks(tasks_dir):
        for variant in variants:
            for model in models:
                cells.append({
                    "run_key": f"{task['id']}|{variant}|{model}",
                    "task": task, "variant": variant, "model": model,
                })
    return cells


# Outcomes that say nothing about selection and may be worth re-spending a
# call on once the provider recovers. Held out of the default resume so a
# re-run stays a no-op; opted in with `run --retry-provider-failures`.
PROVIDER_FAILURE_OUTCOMES = frozenset({
    "provider_quota", "provider_timeout", "provider_error",
    "malformed_provider_stream", "empty_answer",
})

# Every report carries this: args plausibility is a CONTRACT judgement
# against the committed manifest, not a runtime one against the mock.
REPORT_CAVEATS = [
    "args_plausible is validated against comprehension/surface/tool-surface.json "
    "input_schema, NOT the schema the mock serves: the mock serves an empty "
    "permissive inputSchema for every tool (follow-up 01M1W40XF2E0HKT05SS51A5PJ2), "
    "so a call the mock accepted can still grade implausible.",
    "Only the FIRST call's args are graded; a native:* first call has "
    "args_plausible=null.",
    "denominator excludes every non-selection outcome (see `excluded`); report n "
    "alongside every rate.",
]


def is_provider_failure(record: dict) -> bool:
    outcome = record.get("outcome", "")
    return outcome in PROVIDER_FAILURE_OUTCOMES or outcome.startswith("exit_")


# ------------------------------------------------------------------ resume ledger

def read_records(records_path: Path) -> dict[str, dict]:
    out: dict[str, dict] = {}
    if Path(records_path).exists():
        for line in Path(records_path).read_text().splitlines():
            line = line.strip()
            if line:
                record = json.loads(line)
                out[record["run_key"]] = record
    return out


def run_cells(cells, records_path: Path, execute,
              retry_provider_failures: bool = False) -> list[dict]:
    """Execute the cells not already recorded; append each result once.

    With `retry_provider_failures`, a recorded provider-side outcome is run
    again and the new record appended; `read_records` keeps the LAST record
    per run_key, so a recovered cell supersedes the failed one.
    """
    records_path = Path(records_path)
    records_path.parent.mkdir(parents=True, exist_ok=True)
    done = read_records(records_path)
    results = []
    for cell in cells:
        prior = done.get(cell["run_key"])
        if prior is not None and not (retry_provider_failures
                                      and is_provider_failure(prior)):
            results.append(prior)
            continue
        record = execute(cell)
        record["run_key"] = cell["run_key"]
        with records_path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(record, sort_keys=True) + "\n")
        done[cell["run_key"]] = record
        results.append(record)
    return results


# --------------------------------------------------------------------- live cell

def live_executor(grader, opencode_bin: str, timeout: float, out_root: Path):
    def execute(cell) -> dict:
        parent = out_root / "workspaces"
        parent.mkdir(parents=True, exist_ok=True)
        call_log = Path(tempfile.mkstemp(prefix="calls-", suffix=".jsonl",
                                         dir=str(parent))[1])
        workspace = sr.workspace.build_workspace(
            parent, variant_path(cell["variant"]), call_log)
        try:
            result = opencode_runner.run_opencode(
                opencode_bin, workspace, MODELS[cell["model"]],
                SYSTEM_PREFIX + cell["task"]["prompt"], timeout)
            calls = sr.trace.parse_trace(result["raw_stdout"].splitlines())
            record = grader.grade(cell["task"], calls, result["status"])
            record.update({
                "variant": cell["variant"], "model": cell["model"],
                "model_id": MODELS[cell["model"]],
                "wall_seconds": result.get("wall_seconds"),
                "tokens": result.get("tokens"),
                # Cross-check: what the server itself saw, independent of the
                # client's event stream.
                "mock_calls": [json.loads(l) for l in
                               call_log.read_text().splitlines() if l.strip()]
                if call_log.exists() else [],
            })
            if isinstance(record.get("matrix_cell"), tuple):
                record["matrix_cell"] = list(record["matrix_cell"])
            return record
        finally:
            shutil.rmtree(workspace, ignore_errors=True)
    return execute


# --------------------------------------------------------------------- commands

def cmd_plan(args) -> int:
    cells = plan_cells(args.tasks_dir, args.variants, args.models)
    for cell in cells:
        print(cell["run_key"])
    print(f"{len(cells)} cell(s)")
    return 0


def cmd_run(args) -> int:
    live = args.live or os.environ.get("SELECTION_AB_LIVE") == "1"
    opencode_bin = shutil.which(args.opencode_bin)
    reasons = []
    if not opencode_bin:
        reasons.append(f"opencode {args.opencode_bin!r} not found on PATH")
    if not live:
        reasons.append("not opted in (pass --live or SELECTION_AB_LIVE=1)")
    if reasons:
        print("RUN SKIPPED: " + "; ".join(reasons))
        return 0
    cells = plan_cells(args.tasks_dir, args.variants, args.models)
    if args.task:
        cells = [c for c in cells if c["task"]["id"] == args.task]
        if not cells:
            print(f"error: no task matched {args.task!r}", file=sys.stderr)
            return 2
    grader = sr.grade.Grader.from_manifest(MANIFEST)
    execute = live_executor(grader, opencode_bin, args.timeout,
                            Path(args.records).parent)
    for record in run_cells(cells, Path(args.records), execute,
                            retry_provider_failures=args.retry_provider_failures):
        print(f"{record['run_key']}: {record['outcome']} "
              f"first={record.get('first_called_tool')}")
    return 0


def cmd_report(args) -> int:
    records = list(read_records(Path(args.records)).values())
    print(json.dumps({
        "records": len(records),
        "caveats": REPORT_CAVEATS,
        "matrices": sr.matrix.confusion_matrices(records),
    }, indent=2, sort_keys=True))
    return 0


def main(argv=None) -> int:
    # Grid options are shared by every subcommand and accepted on either side
    # of it, so `plan --variants a` and `--variants a plan` both work.
    shared = argparse.ArgumentParser(add_help=False)
    shared.add_argument("--tasks-dir", type=Path, default=TASKS_DIR)
    shared.add_argument("--variants", nargs="+", default=["a", "b", "c", "d"])
    shared.add_argument("--models", nargs="+", default=["weak", "strong"])
    shared.add_argument("--records", type=Path, default=DEFAULT_RECORDS)
    parser = argparse.ArgumentParser(description=__doc__, parents=[shared],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("plan", parents=[shared]).set_defaults(func=cmd_plan)
    run = sub.add_parser("run", parents=[shared])
    run.add_argument("--live", action="store_true")
    run.add_argument("--task", default=None)
    run.add_argument("--opencode-bin", default="opencode")
    run.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    run.add_argument("--retry-provider-failures", action="store_true",
                     help="re-run recorded provider_*/exit_*/empty_answer cells")
    run.set_defaults(func=cmd_run)
    sub.add_parser("report", parents=[shared]).set_defaults(func=cmd_report)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
