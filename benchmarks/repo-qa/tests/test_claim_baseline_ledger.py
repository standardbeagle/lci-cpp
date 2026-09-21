"""Contract tests for the stage-2 claim-validation baseline ledger generator.

Hermetic + network-free. These pin the acceptance criteria of the S2.5
"publish the stage-2 report" task as *deterministic, no-provider* facts:

  * the generator materialises the FULL planned two-arm grid -- every committed
    claim x both arms x every repetition -- and never silently omits a cell;
  * with no paid run approved, every cell carries the machine-readable
    ``not_executed`` status (an un-run cell is recorded, not fabricated into a
    zero-scored answer);
  * the scorer turns that all-missing ledger into a correctly-empty comparison:
    zero paired cells, empty deltas, and no arm's rate inflated by an absent
    divisor;
  * the committed run-config provenance (arms, corpora, task count, digests)
    matches the live runner toolsets, the corpora registry, and the task bank.

A passing one-claim/two-arm claim smoke through the real runner primitive (a
deterministic fake agent) is included so the pipeline wiring is exercised
end-to-end -- the stage-2 instrument genuinely scores a grounded verdict.
"""

import json
import os
import sys
import unittest
from tempfile import TemporaryDirectory

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH_ROOT = os.path.dirname(HERE)
EXPLORATION_ROOT = os.path.join(BENCH_ROOT, "exploration")
SCRIPTS = os.path.join(BENCH_ROOT, "scripts")
for _p in (EXPLORATION_ROOT, SCRIPTS):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import claim_baseline_ledger as ledger  # noqa: E402
from runner import run, toolsets  # noqa: E402
from scoring import score_claim_run  # noqa: E402

CONFIG_PATH = os.path.join(EXPLORATION_ROOT, "run-configs", "stage2-baseline.json")
TASKS_DIR = os.path.join(EXPLORATION_ROOT, "tasks")
CORPORA_PATH = os.path.join(EXPLORATION_ROOT, "corpora.json")


def _load_tasks():
    tasks = {}
    for name in sorted(os.listdir(TASKS_DIR)):
        if name.endswith(".json"):
            tasks[json.load(open(os.path.join(TASKS_DIR, name)))["id"]] = \
                json.load(open(os.path.join(TASKS_DIR, name)))
    return tasks


class GeneratorLedgerTest(unittest.TestCase):
    def setUp(self):
        self.tasks = _load_tasks()

    def test_records_every_planned_cell_none_omitted(self):
        config = json.load(open(CONFIG_PATH))
        reps = config["repetitions"]["per_cell"]
        with TemporaryDirectory() as out:
            report = ledger.build(
                CONFIG_PATH, TASKS_DIR,
                os.path.join(out, "run-records.jsonl"), out)
            recs = [json.loads(line) for line in
                    open(os.path.join(out, "run-records.jsonl")) if line.strip()]
        expected = {f"{tid}::{arm}::seed-7::mode-{run.CLAIM_VALIDATION_MODE}"
                    f"::schema-{run.CLAIM_VALIDATION_SCHEMA}"
                    for tid in self.tasks for arm in ("treatment", "baseline")}
        got = {r["run_key"] for r in recs}
        self.assertEqual(expected, got)
        self.assertEqual(len(recs), len(self.tasks) * 2 * reps)
        self.assertTrue(all(r["status"] == "not_executed" for r in recs))

    def test_claim_record_carries_scoreable_contract(self):
        with TemporaryDirectory() as out:
            ledger.build(CONFIG_PATH, TASKS_DIR,
                         os.path.join(out, "run-records.jsonl"), out)
            recs = [json.loads(line) for line in
                    open(os.path.join(out, "run-records.jsonl")) if line.strip()]
        for rec in recs:
            self.assertEqual(rec["mode"], run.CLAIM_VALIDATION_MODE)
            self.assertEqual(rec["schema_version"], run.CLAIM_VALIDATION_SCHEMA)
            task_id = rec["sealed_metadata"]["task_id"]
            task = self.tasks[task_id]
            self.assertEqual(rec["claim_task_digest"],
                             run._claim_digest(task, run.CLAIM_VALIDATION_SCHEMA))
            self.assertEqual(rec["effective_allowlist"],
                             list(toolsets.arm_allowlist(rec["arm"])))

    def test_all_missing_ledger_scores_to_a_correctly_empty_comparison(self):
        with TemporaryDirectory() as out:
            ledger.build(CONFIG_PATH, TASKS_DIR,
                         os.path.join(out, "run-records.jsonl"), out)
            aggregate = json.load(open(os.path.join(out, "aggregate.json")))
        self.assertEqual(aggregate["schema"], "claim_validation_aggregate_v1")
        self.assertEqual(aggregate["pairing"]["paired_count"], 0)
        self.assertEqual(aggregate["pairing"]["unpaired_count"],
                         len(self.tasks) * 2)
        self.assertEqual(aggregate["deltas"], {})
        for arm in ("treatment", "baseline"):
            summary = aggregate["arms"][arm]
            self.assertEqual(summary["answered"], 0)
            self.assertEqual(summary["verdict_accuracy"], 0.0)
            self.assertEqual(summary["grounded_accuracy"], 0.0)
            self.assertIsNone(summary["citation"]["f1"])
        self.assertEqual(aggregate["compatibility"]["mode"],
                         run.CLAIM_VALIDATION_MODE)

    def test_build_is_deterministic_byte_identical(self):
        with TemporaryDirectory() as a, TemporaryDirectory() as b:
            ledger.build(CONFIG_PATH, TASKS_DIR,
                         os.path.join(a, "run-records.jsonl"), a)
            ledger.build(CONFIG_PATH, TASKS_DIR,
                         os.path.join(b, "run-records.jsonl"), b)
            for name in ("run-records.jsonl", "scores.json", "aggregate.json"):
                self.assertEqual(open(os.path.join(a, name), "rb").read(),
                                 open(os.path.join(b, name), "rb").read(), name)


class RunConfigProvenanceTest(unittest.TestCase):
    def setUp(self):
        self.config = json.load(open(CONFIG_PATH))
        self.corpora = {c["id"]: c
                        for c in json.load(open(CORPORA_PATH))["corpora"]}

    def test_arms_match_live_toolsets(self):
        for arm in ("treatment", "baseline"):
            self.assertEqual(self.config["arms"][arm]["allowlist"],
                             list(toolsets.arm_allowlist(arm)))

    def test_corpora_match_registry(self):
        for entry in self.config["corpora"]:
            ref = self.corpora[entry["corpus_id"]]
            self.assertEqual(entry["source_commit"], ref["pinned_commit"])
            self.assertEqual(
                entry["reference_tree_hash"],
                ref["reference_tree_hash"][f"seed-{entry['seed']}"])

    def test_task_bank_and_signoff_fields_recorded(self):
        self.assertEqual(self.config["provenance"]["task_bank"]["task_count"],
                         len(_load_tasks()))
        self.assertEqual(self.config["repetitions"]["per_cell"], 1)
        self.assertEqual(self.config["seeds"], [7])
        # AC: every paid-run signoff dimension is recorded (even when the grant
        # is refused) so a later approver sees a concrete, verifiable pin.
        approval = self.config["approval"]
        self.assertEqual(approval["status"], "not_granted")
        for key in ("model", "repetitions", "budget", "seeds", "corpora",
                    "task_bank_digest", "arms"):
            self.assertIn(key, self.config)
        self.assertTrue(self.config["task_bank_digest"].startswith("sha256:"))
        self.assertEqual(self.config["approval"]["execution_state"],
                         "pinned_not_executed")


class TwoArmClaimSmokeTest(unittest.TestCase):
    """One claim, both arms, through the real runner + scorer (fake agent)."""

    def test_grounding_answer_scores_success_on_both_arms(self):
        sys.path.insert(0, os.path.join(HERE))
        import exploration_corpus_forge as forge
        from test_exploration_runner import FakeAgent, forge_fixture, base_config
        answer = json.dumps({
            "verdict": "true",
            "evidence": [{"path": "apis/base.go", "line": 3}],
            "rationale": "NewRouter wires the handler",
        })
        task = {
            "id": "smoke-claim", "schema": "exploration_task_v1",
            "claim": "The routing behaviour exists.", "request": "Confirm it.",
            "manifest_ref": {"corpus_id": "synthetic-corpus",
                             "source_commit": "0" * 40, "seed": 7,
                             "forge_version": forge.FORGE_VERSION},
            "author": {"verdict": "true", "category": "true",
                       "anchor_classification": ["authoritative-live"]},
            "evidence": [{"path": "apis/base.go", "lines": [3]}],
        }
        with TemporaryDirectory() as root:
            forge_fixture(root)
            records = os.path.join(root, "records.jsonl")
            results = run.run_task_both_arms(
                task, lambda arm: FakeAgent(_ok(answer)), base_config(),
                corpus_root=root, records_path=records,
                work_root=os.path.join(root, "work"),
                mode=run.CLAIM_VALIDATION_MODE)
        scores = []
        for arm, rec in results.items():
            self.assertEqual(rec["status"], "answered", arm)
            scores.append(score_claim_run(task, rec, task_bank_digest="bank"))
        self.assertTrue(all(s["success"] for s in scores))


def _ok(final_answer):
    from runner.adapter import AgentResult
    return AgentResult(status_hint="ok", final_answer=final_answer,
                       tool_calls=(), input_tokens=5, output_tokens=3,
                       transcript={"events": []})


if __name__ == "__main__":
    unittest.main()
