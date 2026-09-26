#!/usr/bin/env python3
"""Run the D4 two-arm discovery sweep over the pre-registered families.

Two arms (DISJOINT, per predictions.json: the treatment has no Grep/Glob, the
baseline has no LCI) x two measurement levels (direct tool call, and an agent
run) x every cell of every registered family. Grading is set-wise against the
D1 gopls answer key; the runner package holds the parts that must be provable
without a model -- including the routing table and the payload renderer that put
BOTH arms' raw tool output into the grader's `path:line` syntax -- and this
script only supplies the executors that touch a real MCP server, a real grep,
and a real agent.

Tool routing is probed against the live MCP surface, never read off
predictions.json's arms list, which names `mcp__lci__callers` and
`mcp__lci__references`: `callers` IS live and returns call sites with paths;
only `references` is absent (D3 follow-up 01M1VR4TAQ2W3N380QVG6WHQQQ).

  # plan only -- no execution, prints the grid and any cohort reduction
  discovery_sweep.py plan --registry ... --cohorts ...

  # tool level only (cheap, deterministic, no agent, no provider spend)
  discovery_sweep.py run --levels tool --corpus <checkout> --out results/d4

  # both levels, one family, one cell -- the smoke shape
  discovery_sweep.py run --families callers_call_site_high --max-cells 1 ...

Resume is automatic: a cell whose result file exists is not re-run, so an
interrupted sweep continues by re-issuing the same command.
"""

import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchlib as bl  # noqa: E402
import bench  # noqa: E402
from tooleval import McpSession, response_is_error  # noqa: E402

BENCH_ROOT = bl.BENCH_ROOT

import importlib
import importlib.util

# `discovery/runner` and `exploration/runner` are both named `runner`. Putting
# either directory on sys.path shadows the other for the whole process, so this
# package is loaded from its path under a unique module name instead.
_RUNNER_DIR = os.path.join(BENCH_ROOT, "discovery", "runner")


def _load_discovery_runner():
    if "discovery_runner" not in sys.modules:
        spec = importlib.util.spec_from_file_location(
            "discovery_runner",
            os.path.join(_RUNNER_DIR, "__init__.py"),
            submodule_search_locations=[_RUNNER_DIR],
        )
        module = importlib.util.module_from_spec(spec)
        sys.modules["discovery_runner"] = module
        spec.loader.exec_module(module)
    return [importlib.import_module("discovery_runner." + name)
            for name in ("answer_sets", "cells", "grading", "rendering", "sweep")]


_, cells, _grading, rendering, sweep = _load_discovery_runner()

DISCOVERY_DIR = os.path.join(bl.BENCH_ROOT, "discovery")
DEFAULT_REGISTRY = os.path.join(DISCOVERY_DIR, "predictions.json")
DEFAULT_COHORTS = os.path.join(DISCOVERY_DIR, "cohorts", "cohorts.json")
DEFAULT_ORACLE_CACHE = os.path.join(DISCOVERY_DIR, "oracle", "cache")


def load_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def oracle_key_lookup(corpus_root, cache_dir):
    """Answer keys straight from the D1 oracle, which fails loud on its own."""
    from gopls_oracle import GoplsOracle, SymbolAnchor

    oracle = GoplsOracle.for_corpus(corpus_root=corpus_root, cache_dir=cache_dir)

    def lookup(cell):
        anchor = SymbolAnchor(
            name=cell["name"], path=cell["path"], line=cell["line"],
            column=cell["column"],
        )
        return oracle.answer_key(anchor, max_depth=2)

    return lookup


def _grep_answer(corpus_root, name):
    """The baseline arm's native tool, called directly: one grep, cited."""
    proc = subprocess.run(
        ["grep", "-rn", "--include=*.go", name, "."],
        cwd=corpus_root, capture_output=True, text=True,
    )
    if proc.returncode not in (0, 1):  # 1 = no matches, which is an answer
        return None, proc.stderr.strip()
    lines = []
    for raw in proc.stdout.splitlines():
        parts = raw.split(":", 2)
        if len(parts) >= 2:
            path = parts[0][2:] if parts[0].startswith("./") else parts[0]
            lines.append("%s:%s" % (path, parts[1]))
    return "\n".join(lines), None


def tool_executor(corpus_root, lci_bin):
    """Direct tool call, no agent. Both arms answer through their own surface."""
    session = {"mcp": None}

    def execute(job):
        cell = job["cell"]
        started = time.time()
        if job["arm"] == "baseline":
            answer, error = _grep_answer(corpus_root, cell["name"])
            if error:
                return {"status": "error", "detail": "grep failed: %s" % error}
            return {"status": "ok", "answer": answer, "tool_calls": 1,
                    "wall_seconds": round(time.time() - started, 3)}

        if session["mcp"] is None:
            session["mcp"] = McpSession(lci_bin, corpus_root)
        tool, build_args = rendering.tool_for(job["task_shape"])
        if tool is None:
            return {"status": "error",
                    "detail": "no LCI tool routed for task shape %r"
                              % (job["task_shape"],)}
        payload, latency_ms = session["mcp"].call_tool(tool, build_args(cell))
        if "text" not in payload or response_is_error(payload):
            # An LCI error payload arrives over a successful RPC. Grading its
            # text would score the treatment 0.0 on a cell it was never asked
            # correctly -- exactly the zeroed row that reads as a real loss.
            return {"status": "error",
                    "detail": "%s refused the call: %s" % (tool, str(payload)[:300])}
        try:
            # The baseline's grep output is rendered into `path:line`; the
            # treatment's JSON must be too, or it cannot cite by construction.
            answer = rendering.render_tool_answer(
                tool, job["task_shape"], payload["text"])
        except rendering.UncitableToolResponse as exc:
            # A named harness-known outcome (truncated_response, uncitable_
            # location), never a zero: the reason rides the DNF into the row's
            # score and the report's ungradable_reasons, and the sweep runs on.
            return {"status": "dnf", "reason": exc.reason,
                    "detail": "%s payload is uncitable: %s"
                              % (tool, exc.detail[:200]),
                    "tool_calls": 1,
                    "wall_seconds": round(latency_ms / 1000.0, 3)}
        return {"status": "ok", "answer": answer, "tool_calls": 1,
                "wall_seconds": round(latency_ms / 1000.0, 3)}

    return execute


def agent_executor(cfg, corpus_root, model_id, timeout):
    """One opencode run per cell, per arm, through bench.py's own mechanism."""
    workspaces = {}

    def workspace(arm):
        if arm not in workspaces:
            variant = "lci" if arm == "treatment" else "base"
            workspaces[arm] = bench.ensure_workspace(cfg, corpus_root, variant)
        return workspaces[arm]

    def execute(job):
        ws = workspace(job["arm"])
        parsed = bench.run_one(
            cfg, ws, model_id, {"question": job["prompt"]}, timeout
        )
        if parsed["status"] in ("timeout", "empty_answer"):
            # Not a broken cell: the arm was given its budget and produced no
            # answer. Prior tiers found completion rate was LCI's only
            # reproducible win, so this outcome is recorded, never dropped.
            return {"status": "dnf", "reason": parsed["status"],
                    "tool_calls": len(parsed["tool_calls"]),
                    "tokens": parsed["tokens"], "wall_seconds": parsed["wall_seconds"]}
        if parsed["status"] != "ok":
            return {"status": "error", "detail": parsed["status"]}
        return {"status": "ok", "answer": parsed["answer"],
                "tool_calls": len(parsed["tool_calls"]),
                "tokens": parsed["tokens"], "wall_seconds": parsed["wall_seconds"]}

    return execute


def build_plan(args):
    registry = load_json(args.registry)
    cohorts = load_json(args.cohorts)
    if args.families:
        wanted = set(args.families.split(","))
        unknown = wanted - {f["id"] for f in registry["families"]}
        if unknown:
            raise SystemExit("error: unknown families: %s" % ", ".join(sorted(unknown)))
        registry = dict(registry)
        registry["families"] = [f for f in registry["families"] if f["id"] in wanted]
    return cells.build_plan(registry, cohorts, max_cells_per_family=args.max_cells)


def cmd_plan(args):
    plan = build_plan(args)
    for family in plan["families"]:
        print("%-38s %-32s %d cells" % (family["id"], family["task_shape"],
                                        len(family["cells"])))
    for entry in plan["voided"]:
        print("VOIDED %s: %s" % (entry["family"], entry["reason"]))
    reduction = plan["cohort_reduction"]
    if reduction["max_cells_per_family"] is not None:
        print("\nCOHORT REDUCTION: max %d cells per family" %
              reduction["max_cells_per_family"])
        for family, record in sorted(reduction["families"].items()):
            print("  %s: ran %d of %d (dropped %s)" %
                  (family, record["ran_n"], record["declared_n"],
                   ", ".join(record["dropped"])))
    return 0


def cmd_run(args):
    plan = build_plan(args)
    plan["levels"] = args.levels.split(",")
    cfg = bl.Config()
    corpus_root = os.path.abspath(args.corpus)
    lci_bin = args.lci_bin or cfg.defaults["lci-bin"]

    tool = tool_executor(corpus_root, lci_bin)
    agent = None
    if "agent" in plan["levels"]:
        if not args.model:
            raise SystemExit("error: --model is required to run the agent level")
        agent = agent_executor(cfg, args.corpus_repo or corpus_root,
                               args.model, args.timeout)

    def execute(job):
        return tool(job) if job["level"] == "tool" else agent(job)

    out_dir = args.out if os.path.isabs(args.out) else os.path.join(bl.BENCH_ROOT, args.out)
    counters = sweep.run_sweep(
        plan,
        answer_key_for=oracle_key_lookup(corpus_root, args.oracle_cache),
        executor=execute,
        run_dir=out_dir,
        literal_corpus_root=corpus_root,
    )
    print("executed=%(executed)d resumed=%(resumed)d dnf=%(dnf)d" % counters)
    report = sweep.build_report(out_dir)
    report_path = os.path.join(out_dir, "report.json")
    with open(report_path, "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2, sort_keys=True)
    print("report: %s" % report_path)
    if report["tool_correct_no_agent_lift"]:
        print("TOOL-CORRECT BUT NO AGENT LIFT: %s" %
              ", ".join(report["tool_correct_no_agent_lift"]))
    return 0


def cmd_report(args):
    report = sweep.build_report(args.out if os.path.isabs(args.out)
                                else os.path.join(bl.BENCH_ROOT, args.out))
    json.dump(report, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    for name in ("plan", "run"):
        cmd = sub.add_parser(name)
        cmd.add_argument("--registry", default=DEFAULT_REGISTRY)
        cmd.add_argument("--cohorts", default=DEFAULT_COHORTS)
        cmd.add_argument("--families", help="comma-separated family ids")
        cmd.add_argument(
            "--max-cells", type=int, default=None,
            help="cohort-size reduction knob; recorded per family in the report, "
                 "with the dropped slugs named. Never a silent family cap.",
        )

    run = sub.choices["run"]
    run.add_argument("--corpus", required=True, help="the graded checkout (oracle + tool level)")
    run.add_argument("--corpus-repo", help="bench config repo alias for the agent level")
    run.add_argument("--levels", default="tool,agent")
    run.add_argument("--out", required=True)
    run.add_argument("--lci-bin")
    run.add_argument("--model", help="full provider/model id for the agent level")
    run.add_argument("--timeout", type=int, default=300)
    run.add_argument("--oracle-cache", default=DEFAULT_ORACLE_CACHE)

    rep = sub.add_parser("report")
    rep.add_argument("--out", required=True)

    args = parser.parse_args(argv)
    return {"plan": cmd_plan, "run": cmd_run, "report": cmd_report}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
