#!/usr/bin/env python3
"""Run Stage-1 exploration tasks under both arms and record per-run traces.

Two subcommands:

  run    Batch over the task bank with the real Claude CLI adapter, appending
         one resume-idempotent record per (task, arm). Requires the forged
         corpora on disk (exploration_corpus_forge.py forge ...).

  smoke  One task, both arms, live wiring check. Guarded: it SKIPS (exit 0)
         unless the Claude CLI + the LCI binary are present and the run is
         opted in with --live or EXPLORATION_RUNNER_LIVE=1. This is the only
         path that spends provider credentials; the unit suite never does.

The Claude invocation lives behind the injectable `runner.adapter` interface,
so the same orchestration (`runner.run.run_task`) that unit tests drive with a
fake agent is what these subcommands drive with the real CLI.
"""

import argparse
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH_ROOT = os.path.dirname(HERE)
EXPLORATION_ROOT = os.path.join(BENCH_ROOT, "exploration")
EDITS_RUNNER = os.path.join(BENCH_ROOT, "edits", "runner")
for _p in (EXPLORATION_ROOT, HERE, EDITS_RUNNER):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import exploration_corpus_forge as forge  # noqa: E402
from runner import toolsets  # noqa: E402
from runner.adapter import ClaudeCliAdapter  # noqa: E402
from runner.run import (  # noqa: E402
    CLAIM_VALIDATION_MODE,
    BaseConfig,
    run_task,
    run_task_both_arms,
)

# Edit mode (Stage-3) reuses this same paired runner: same arms + isolation +
# record log, extended with the mutation toolset and the behaviour/convention
# gates. Imported here so `edit-run` / `edit-smoke` share the CLI wiring.
import edit_run  # noqa: E402
import edit_toolsets  # noqa: E402

DEFAULT_EDIT_TASKS_DIR = os.path.join(BENCH_ROOT, "edits", "tasks")
EDIT_SYSTEM_PROMPT = edit_toolsets.EDIT_SYSTEM_PROMPT

DEFAULT_TASKS_DIR = os.path.join(EXPLORATION_ROOT, "tasks")
DEFAULT_CORPUS_ROOT = forge.DEFAULT_OUT_ROOT
DEFAULT_WORK_ROOT = os.path.join(forge.DEFAULT_OUT_ROOT, "runs")
DEFAULT_RECORDS = os.path.join(forge.DEFAULT_OUT_ROOT, "runs", "records.jsonl")
DEFAULT_MODEL = "claude-sonnet-4-5"
DEFAULT_SYSTEM_PROMPT = (
    "You are exploring an unfamiliar code checkout to answer a question about "
    "where specific behaviour lives. Ground every claim in a concrete file and "
    "location you found in THIS checkout; do not answer from prior memory of any "
    "upstream project. Conclude with the answer the question asks for."
)


def _load_tasks(tasks_dir, only=None):
    tasks = []
    for name in sorted(os.listdir(tasks_dir)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(tasks_dir, name), encoding="utf-8") as handle:
            task = json.load(handle)
        if only and task.get("id") != only:
            continue
        tasks.append(task)
    return tasks


def _base_config(args):
    return BaseConfig(
        model=args.model,
        system_prompt=args.system_prompt,
        timeout_seconds=args.timeout,
    )


def _lci_mcp_config(lci_bin):
    """Write a Claude --mcp-config JSON registering the LCI server over stdio.

    The server is launched by the CLI with cwd set to the run's checkout, so it
    indexes exactly the clean corpus copy the agent is exploring.
    """
    config = {"mcpServers": {"lci": {"command": lci_bin, "args": ["mcp"]}}}
    handle, path = tempfile.mkstemp(prefix="lci-mcp-", suffix=".json")
    with os.fdopen(handle, "w", encoding="utf-8") as out:
        json.dump(config, out)
    return path


def _cmd_run(args):
    base = _base_config(args)
    adapter = ClaudeCliAdapter(
        claude_bin=args.claude_bin,
        mcp_config=_lci_mcp_config(args.lci_bin),
    )
    arms = (toolsets.TREATMENT, toolsets.BASELINE) if args.arm == "both" else (args.arm,)

    def adapter_for(arm):
        # Baseline gets no LCI MCP server at all -- belt-and-braces with the gate.
        if arm == toolsets.BASELINE:
            return ClaudeCliAdapter(claude_bin=args.claude_bin)
        return adapter

    count = 0
    for task in _load_tasks(args.tasks_dir, only=args.task):
        for arm in arms:
            rec = run_task(
                task, arm, adapter_for(arm), base,
                corpus_root=args.corpus_root,
                records_path=args.records,
                work_root=args.work_root,
                mode=getattr(args, "runner_mode", "exploration"),
            )
            count += 1
            marker = "skip" if rec.get("skipped") else rec.get("status")
            print(f"{rec['run_key']}: {marker}")
    print(f"{count} run(s) -> {args.records}")
    return 0


def _cmd_smoke(args):
    claude_bin = shutil.which(args.claude_bin)
    lci_bin = args.lci_bin if os.path.isfile(args.lci_bin) else shutil.which(args.lci_bin)
    live = args.live or os.environ.get("EXPLORATION_RUNNER_LIVE") == "1"

    reasons = []
    if not claude_bin:
        reasons.append(f"claude CLI {args.claude_bin!r} not found")
    if not lci_bin:
        reasons.append(f"lci binary {args.lci_bin!r} not found")
    if not live:
        reasons.append("not opted in (pass --live or EXPLORATION_RUNNER_LIVE=1)")
    if reasons:
        print("SMOKE SKIPPED: " + "; ".join(reasons))
        return 0

    tasks = _load_tasks(args.tasks_dir, only=args.task)
    if not tasks:
        print(f"error: no task matched {args.task!r} in {args.tasks_dir}", file=sys.stderr)
        return 2
    task = tasks[0]
    base = _base_config(args)
    mcp_config = _lci_mcp_config(lci_bin)

    def adapter_for(arm):
        if arm == toolsets.BASELINE:
            return ClaudeCliAdapter(claude_bin=claude_bin)
        return ClaudeCliAdapter(claude_bin=claude_bin, mcp_config=mcp_config)

    results = run_task_both_arms(
        task, adapter_for, base,
        corpus_root=args.corpus_root,
        records_path=args.records,
        work_root=args.work_root,
        mode=getattr(args, "runner_mode", "exploration"),
    )
    for arm, rec in results.items():
        print(f"{arm}: {rec.get('status')} ({len(rec.get('tool_calls', []))} tool calls)")
    return 0


def _edit_base_config(args):
    # Edit mode uses its own generic system prompt (no oracle material) unless
    # the caller overrides it; everything else is shared with exploration.
    prompt = args.system_prompt
    if prompt is DEFAULT_SYSTEM_PROMPT:
        prompt = EDIT_SYSTEM_PROMPT
    return BaseConfig(
        model=args.model, system_prompt=prompt, timeout_seconds=args.timeout
    )


def _edit_adapter_for(arm, args, mcp_config):
    # Both arms MAY edit (disallow only Bash); the treatment arm additionally
    # gets the LCI MCP server. The runner's gate re-checks every emitted call.
    if arm == edit_toolsets.BASELINE:
        return ClaudeCliAdapter(
            claude_bin=args.claude_bin, disallowed_tools=("Bash",)
        )
    return ClaudeCliAdapter(
        claude_bin=args.claude_bin, mcp_config=mcp_config,
        disallowed_tools=("Bash",),
    )


def _cmd_edit_run(args):
    base = _edit_base_config(args)
    mcp_config = _lci_mcp_config(args.lci_bin)
    tasks = _load_tasks(args.tasks_dir, only=args.task)
    count = 0
    for rec in edit_run.run_edit_bank(
        tasks,
        lambda arm: _edit_adapter_for(arm, args, mcp_config),
        base,
        corpus_root=args.corpus_root,
        records_path=args.records,
        work_root=args.work_root,
    ):
        count += 1
        marker = "skip" if rec.get("skipped") else rec.get("status")
        print(f"{rec['run_key']}: {marker}")
    print(f"{count} edit run(s) -> {args.records}")
    return 0


def _cmd_edit_smoke(args):
    claude_bin = shutil.which(args.claude_bin)
    lci_bin = args.lci_bin if os.path.isfile(args.lci_bin) else shutil.which(args.lci_bin)
    live = args.live or os.environ.get("EXPLORATION_RUNNER_LIVE") == "1"

    reasons = []
    if not claude_bin:
        reasons.append(f"claude CLI {args.claude_bin!r} not found")
    if not lci_bin:
        reasons.append(f"lci binary {args.lci_bin!r} not found")
    if not live:
        reasons.append("not opted in (pass --live or EXPLORATION_RUNNER_LIVE=1)")
    if reasons:
        print("EDIT SMOKE SKIPPED: " + "; ".join(reasons))
        return 0

    tasks = _load_tasks(args.tasks_dir, only=args.task)
    if not tasks:
        print(f"error: no task matched {args.task!r} in {args.tasks_dir}", file=sys.stderr)
        return 2
    task = tasks[0]
    base = _edit_base_config(args)
    mcp_config = _lci_mcp_config(lci_bin)
    results = edit_run.run_edit_task_both_arms(
        task,
        lambda arm: _edit_adapter_for(arm, args, mcp_config),
        base,
        corpus_root=args.corpus_root,
        records_path=args.records,
        work_root=args.work_root,
    )
    for arm, rec in results.items():
        print(f"{arm}: {rec.get('status')} ({len(rec.get('patch_files') or [])} file(s))")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tasks-dir", default=DEFAULT_TASKS_DIR)
    parser.add_argument("--corpus-root", default=DEFAULT_CORPUS_ROOT)
    parser.add_argument("--records", default=DEFAULT_RECORDS)
    parser.add_argument("--work-root", default=DEFAULT_WORK_ROOT)
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--system-prompt", default=DEFAULT_SYSTEM_PROMPT)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--claude-bin", default="claude")
    parser.add_argument(
        "--lci-bin",
        default=os.path.join(
            os.path.dirname(os.path.dirname(BENCH_ROOT)),
            "build", "release", "src", "lci",
        ),
    )
    sub = parser.add_subparsers(dest="command", required=True)

    run = sub.add_parser("run", help="batch the task bank (real adapter)")
    run.add_argument("--task", default=None, help="restrict to one task id")
    run.add_argument("--arm", choices=[toolsets.TREATMENT, toolsets.BASELINE, "both"], default="both")
    run.set_defaults(func=_cmd_run)

    claim_run = sub.add_parser(
        "claim-run", help="batch isolated structured claim validation"
    )
    claim_run.add_argument("--task", default=None, help="restrict to one task id")
    claim_run.add_argument(
        "--arm", choices=[toolsets.TREATMENT, toolsets.BASELINE, "both"], default="both"
    )
    claim_run.set_defaults(func=_cmd_run, runner_mode=CLAIM_VALIDATION_MODE)

    smoke = sub.add_parser("smoke", help="one task/two arms, guarded live check")
    smoke.add_argument("--task", default=None, help="task id (default: first)")
    smoke.add_argument("--live", action="store_true", help="opt in to a real run")
    smoke.set_defaults(func=_cmd_smoke)

    claim_smoke = sub.add_parser(
        "claim-smoke", help="one claim/two arms, guarded live structured check"
    )
    claim_smoke.add_argument("--task", default=None, help="task id (default: first)")
    claim_smoke.add_argument("--live", action="store_true", help="opt in to a real run")
    claim_smoke.set_defaults(func=_cmd_smoke, runner_mode=CLAIM_VALIDATION_MODE)

    edit_run_cmd = sub.add_parser(
        "edit-run", help="batch the EDIT task bank, both arms (real adapter)"
    )
    edit_run_cmd.add_argument(
        "--tasks-dir", default=DEFAULT_EDIT_TASKS_DIR, dest="tasks_dir"
    )
    edit_run_cmd.add_argument("--task", default=None, help="restrict to one task id")
    edit_run_cmd.set_defaults(func=_cmd_edit_run)

    edit_smoke = sub.add_parser(
        "edit-smoke", help="one EDIT task/two arms, guarded live check"
    )
    edit_smoke.add_argument(
        "--tasks-dir", default=DEFAULT_EDIT_TASKS_DIR, dest="tasks_dir"
    )
    edit_smoke.add_argument("--task", default=None, help="task id (default: first)")
    edit_smoke.add_argument("--live", action="store_true", help="opt in to a real run")
    edit_smoke.set_defaults(func=_cmd_edit_smoke)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
