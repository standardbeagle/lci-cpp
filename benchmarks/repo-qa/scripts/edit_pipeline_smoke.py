"""Stage-3 edit-benchmark two-arm smoke: the pre-flight for any paid grid.

This is a MEASUREMENT-PLUMBING proof, not a measurement. It exercises the exact
code path the paid grid uses -- ``run_edit_task_both_arms`` -> real
``prepare_checkout`` -> tool-isolation ``gate.enforce`` -> ``capture_patch`` ->
the real ``oracle_gate`` and ``conformance_gate`` -> the committed record log --
over ONE task in BOTH arms, and then feeds the two real records through the
real scorer and aggregate. No model, no credentials, no provider call: a
deterministic fake agent performs a conforming edit and the behaviour command
is trivial argv, so the run is hermetic and byte-stable.

The smoke PASSES iff BOTH arms:
  * reach a COMPLETED status (the patch was captured and BOTH gates rendered a
    verdict -- i.e. neither arm is a config_error / harness_failure / timeout /
    agent_failure, which would mean the instrument itself is broken); and
  * their records are schema-validatable by ``edit_scorer.validate_score`` and
    the resulting two-cell aggregate validates against ``edit_aggregate_v1``.

A green smoke is a NECESSARY-but-not-sufficient precondition for the paid run:
it shows the grid would be scored honestly if it ran. It says nothing about
whether LCI helps (that is the paid grid's job). Run it BEFORE launching paid
cells; if it fails, the grid must NOT be launched.

Reproducible:
    python3 benchmarks/repo-qa/scripts/edit_smoke.py --out-dir <dir>

Writes ``<dir>/run-records.jsonl`` (the 2 smoke records, ephemeral paths
normalised), ``<dir>/scores.json`` and ``<dir>/aggregate.json``, then prints a
single PASS/FAIL line. Exit code 0 iff the smoke passes.
"""

import argparse
import copy
import json
import os
import sys
from tempfile import TemporaryDirectory

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

import edit_paths  # noqa: E402,F401  (wires exploration runner + gates)
import edit_record  # noqa: E402
import edit_run  # noqa: E402
import edit_toolsets  # noqa: E402
import exploration_corpus_forge as forge  # noqa: E402
from runner.adapter import AgentResult, FakeAgent  # noqa: E402

import edit_scorer  # noqa: E402

FAKE_COMMIT = "0" * 40
SMOKE_MODEL = "edit-smoke-fake-model"

# Trivial real behaviour command: RED on the pristine tree, GREEN once the
# "GREEN" marker exists. The fake agent writes that marker (plus a conforming
# edit), so both gates render a real verdict with no project toolchain.
BEHAVIOR_CMD = [
    sys.executable, "-c",
    "import os,sys; sys.exit(0 if os.path.isfile('GREEN') else 1)",
]
SUITE_CMD = [sys.executable, "-c", "import sys; sys.exit(0)"]

_CONFORMING_WIDGET = (
    "package apis\n\ntype Widget struct{}\n\n"
    "func NewWidget() *Widget { return &Widget{} }\n"
)


class _StaticLci:
    def available(self):
        return True

    def refs(self, _symbol, _tree_dir):
        return ["apis/base.go"]


def forge_fixture(root, corpus_id="smoke-corpus", seed=7):
    """Synthesise a two-file forged corpus + manifest pinning the tree hash,
    reusing the forge's emit shape so ``corpus.prepare_checkout`` accepts it."""
    corpus_dir = os.path.join(root, corpus_id, f"seed-{seed}")
    tree = os.path.join(corpus_dir, "tree")
    os.makedirs(os.path.join(tree, "apis"))
    with open(os.path.join(tree, "apis", "base.go"), "w") as handle:
        handle.write(
            "package apis\n\ntype Router struct{}\n\n"
            "func NewRouter() *Router { return &Router{} }\n"
        )
    with open(os.path.join(tree, "apis", "store.go"), "w") as handle:
        handle.write(
            "package apis\n\ntype Store struct{}\n\n"
            "func NewStore() *Store { return &Store{} }\n"
        )
    manifest = {
        "schema": forge.MANIFEST_SCHEMA,
        "corpus_id": corpus_id,
        "source_path": "/nonexistent/source",
        "source_commit": FAKE_COMMIT,
        "seed": seed,
        "forge_version": forge.FORGE_VERSION,
        "overrides": [],
        "mutations": [],
        "path_map": {rel: rel for rel in forge.list_files(tree)},
        "decoys": [],
        "tree_hash": forge.tree_hash(tree),
        "validation": {"passed": True},
        "status": "ready",
    }
    forge._write_json_atomic(os.path.join(corpus_dir, "manifest.json"), manifest)
    return corpus_dir, tree, manifest


def smoke_task(corpus_id="smoke-corpus", seed=7, task_id="smoke-pb-module-1"):
    return {
        "schema": "edit_task_v1",
        "id": task_id,
        "corpus": corpus_id,
        "category": "module-extraction-layout",
        "manifest_ref": {
            "corpus_id": corpus_id,
            "source_commit": FAKE_COMMIT,
            "seed": seed,
            "forge_version": forge.FORGE_VERSION,
        },
        "prompt": (
            "Bring the off-pattern constructor in line with how its "
            "neighbours build their types."
        ),
        "rubric": {
            "must_surface": ["returns a pointer from a New* constructor"],
            "answer_shape": "a small patch matching the house constructor style",
        },
        "behavior": {
            "command": ["pnpm", "test"],
            "assertion": "the constructor is exercised",
            "discrimination": {
                "red": "the sentinel value is returned",
                "green": "a pointer is returned by NewThing",
            },
        },
        "convention": {
            "rule_id": "go-constructor-new-pointer",
            "statement": "Constructors are named New<Type> and RETURN A POINTER *T.",
            "conforms_pattern": r"func New\w+\([^)]*\)\s*\*",
        },
        "exemplars": [
            {"path": "apis/base.go", "lines": [5], "claim": "NewRouter returns *Router",
             "target_identifiers": ["NewRouter"]},
            {"path": "apis/store.go", "lines": [5], "claim": "NewStore returns *Store",
             "target_identifiers": ["NewStore"]},
        ],
        "blast_radius": {"allow": ["GREEN", "apis/**", "widget/**"], "max_files": 3},
        "adjudication": {"annotators": ["ann-hyde", "ann-quill"], "resolved": True},
    }


def _pass_agent():
    def _run(request):
        for rel, content in (("GREEN", "ok\n"), ("apis/widget.go", _CONFORMING_WIDGET)):
            dest = os.path.join(request.checkout_dir, rel)
            os.makedirs(os.path.dirname(dest) or dest, exist_ok=True)
            with open(dest, "w", encoding="utf-8") as handle:
                handle.write(content)
        return AgentResult(
            status_hint="ok", final_answer="smoke edit applied",
            tool_calls=(), input_tokens=10, output_tokens=3,
            transcript={"turns": [{"role": "assistant", "text": "smoke edit applied"}]},
        )

    return FakeAgent(_run)


def _normalise(record, work_root):
    """Replace the ephemeral tempdir prefix in transcript_ref so the committed
    smoke ledger is byte-stable across runs/machines.

    Wall-clock is also pinned out: the agent is a deterministic fake, so its
    latency is harness jitter, not a measured quantity. Zeroing it keeps the
    smoke ledger reproducible byte-for-byte without discarding any real signal
    (the paid grid's records, by contrast, keep their genuine timing)."""
    rec = copy.deepcopy(record)
    ref = rec.get("transcript_ref") or ""
    if work_root and ref.startswith(work_root):
        rec["transcript_ref"] = "<smoke-work>" + ref[len(work_root):]
    rec["started_at"] = None
    rec["ended_at"] = None
    rec["duration_seconds"] = 0.0
    return rec


def run_smoke(out_dir):
    task = smoke_task()
    base = edit_run.BaseConfig(
        model=SMOKE_MODEL,
        system_prompt=edit_toolsets.EDIT_SYSTEM_PROMPT,
        timeout_seconds=120,
    )
    with TemporaryDirectory() as root:
        forge_fixture(root, corpus_id=task["manifest_ref"]["corpus_id"], seed=7)
        records_path = os.path.join(out_dir, "run-records.jsonl")
        work_root = os.path.join(root, "work")
        results = edit_run.run_edit_task_both_arms(
            task, lambda _arm: _pass_agent(), base,
            corpus_root=root, records_path=records_path, work_root=work_root,
            behavior_command=BEHAVIOR_CMD, existing_suite_command=SUITE_CMD,
            impacted_symbols=("NewStore",), lci=_StaticLci(),
        )
        raw = [results[arm] for arm in (edit_toolsets.TREATMENT, edit_toolsets.BASELINE)]

    records = [_normalise(rec, work_root) for rec in raw]
    with open(records_path, "w", encoding="utf-8") as handle:
        for rec in records:
            handle.write(json.dumps(rec, sort_keys=True) + "\n")

    # Rewrite the transcript refs inside the scores/aggregate by scoring the
    # normalised records (scorer only reads gates/process/token fields).
    scores = []
    for rec in records:
        score = edit_scorer.score_run(rec)
        edit_scorer.validate_score(score)
        scores.append(score)

    planned = edit_scorer.planned_cells(
        [task["id"]], [edit_toolsets.TREATMENT, edit_toolsets.BASELINE], [7]
    )
    report = edit_scorer.aggregate(scores, planned)
    edit_scorer.validate_aggregate(report)

    with open(os.path.join(out_dir, "scores.json"), "w", encoding="utf-8") as handle:
        json.dump(scores, handle, indent=2, sort_keys=True)
        handle.write("\n")
    with open(os.path.join(out_dir, "aggregate.json"), "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2, sort_keys=True)
        handle.write("\n")

    ok = all(
        rec["status"] in edit_record.COMPLETED_STATUSES and rec["status"] == "edit_passed"
        for rec in records
    ) and report["completeness"]["passed"] and report["headline"]["complete"]
    return ok, records, report


def main(argv=None):
    parser = argparse.ArgumentParser(description="Stage-3 edit two-arm smoke")
    parser.add_argument(
        "--out-dir",
        default=os.path.join(EDITS, "results", "smoke"),
        help="where to write the smoke ledger/scores/aggregate",
    )
    args = parser.parse_args(argv)
    os.makedirs(args.out_dir, exist_ok=True)
    ok, records, report = run_smoke(args.out_dir)
    for rec in records:
        print(f"  {rec['arm']:<9} {rec['status']:<16} {rec['run_key']}")
    if ok:
        print(f"SMOKE PASS: both arms completed (all gates green), "
              f"completeness passed, planned={report['completeness']['planned_cells']} "
              f"observed={report['completeness']['observed_cells']}")
        return 0
    print("SMOKE FAIL: an arm did not reach a green completed verdict, or "
          "completeness/headline failed -- the paid grid must NOT be launched.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
