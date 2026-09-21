"""Contract tests for the published stage-3 edit baseline artifacts.

These lock the invariants that make the stage-3 report honest and reproducible:

  1. the two-arm pipeline smoke reaches edit_passed on BOTH arms and is
     byte-stable across re-runs (the pre-flight is deterministic, no provider);
  2. the committed baseline aggregate re-derives byte-identically from the
     pinned config + (empty) ledger, and reports the full planned grid as
     missing_cell -- not a fabricated answer, not a silent gap;
  3. the run config mechanically pins every field the acceptance criteria
     require (model, repetitions/budget, commits/tree hashes, seeds, schema
     versions, LCI binary, per-arm tool allowlists, approval state);
  4. the not-run grid carries NO measurable effect: empty deltas, zero-filled
     matrix, null process metrics, completeness failed, headline.complete False.

Hermetic + network-free: a fake agent, trivial argv behaviour commands, no
model, no credentials, no dependence on the gitignored .work/ corpora.
"""

import itertools
import json
import os
import sys
import unittest
from tempfile import TemporaryDirectory

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH_ROOT = os.path.dirname(HERE)
EDITS = os.path.join(BENCH_ROOT, "edits")
SCRIPTS = os.path.join(BENCH_ROOT, "scripts")
for _p in (
    os.path.join(EDITS, "runner"),
    os.path.join(EDITS, "scoring"),
    os.path.join(BENCH_ROOT, "exploration"),
    SCRIPTS,
):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import edit_baseline_ledger  # noqa: E402
import edit_pipeline_smoke  # noqa: E402
import edit_toolsets  # noqa: E402

CONFIG = os.path.join(EDITS, "run-config.json")
TASKS_DIR = os.path.join(EDITS, "tasks")
BASELINE_AGG = os.path.join(EDITS, "results", "baseline", "aggregate.json")
BASELINE_RECORDS = os.path.join(EDITS, "results", "baseline", "run-records.jsonl")


def _read_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


class SmokeTest(unittest.TestCase):
    def test_both_arms_pass_and_artifacts_are_byte_stable(self):
        with TemporaryDirectory() as root:
            a = os.path.join(root, "a")
            b = os.path.join(root, "b")
            ok_a, recs_a, agg_a = edit_pipeline_smoke.run_smoke(a)
            ok_b, _recs_b, agg_b = edit_pipeline_smoke.run_smoke(b)
            with open(os.path.join(a, "aggregate.json"), encoding="utf-8") as h:
                text_a = h.read()
            with open(os.path.join(b, "aggregate.json"), encoding="utf-8") as h:
                text_b = h.read()

        self.assertTrue(ok_a, "hermetic two-arm smoke must pass")
        statuses = sorted(r["status"] for r in recs_a)
        self.assertEqual(statuses, ["edit_passed", "edit_passed"])
        self.assertEqual({r["arm"] for r in recs_a}, {edit_toolsets.TREATMENT, edit_toolsets.BASELINE})
        self.assertTrue(agg_a["completeness"]["passed"])
        self.assertTrue(agg_a["headline"]["complete"])
        for arm in (edit_toolsets.TREATMENT, edit_toolsets.BASELINE):
            self.assertEqual(agg_a["headline"]["arms"][arm]["rate"], 1.0)
        # byte-stable: ephemeral wall-clock pinned out
        self.assertEqual(text_a, text_b)
        self.assertEqual(agg_a, agg_b)


class BaselineAggregateTest(unittest.TestCase):
    def test_reproduces_committed_artifact_byte_identically(self):
        with TemporaryDirectory() as root:
            edit_baseline_ledger.build(CONFIG, TASKS_DIR, BASELINE_RECORDS, root)
            self.assertEqual(
                _read_json(os.path.join(root, "aggregate.json")),
                _read_json(BASELINE_AGG),
                "committed aggregate must re-derive deterministically (drift guard)",
            )

    def test_every_planned_cell_is_accounted_as_missing(self):
        agg = _read_json(BASELINE_AGG)
        self.assertEqual(agg["schema"], "edit_aggregate_v1")
        comp = agg["completeness"]
        self.assertFalse(comp["passed"])
        self.assertEqual(comp["planned_cells"], 48)
        self.assertEqual(comp["observed_cells"], 0)
        self.assertEqual(comp["missing_cells"], 48)
        self.assertEqual(comp["unplanned"], [])
        # 24 treatment + 24 baseline missing cells, covering all 24 tasks
        by_arm = {}
        for cell in comp["missing"]:
            by_arm[cell["arm"]] = by_arm.get(cell["arm"], 0) + 1
        self.assertEqual(by_arm[edit_toolsets.TREATMENT], 24)
        self.assertEqual(by_arm[edit_toolsets.BASELINE], 24)
        self.assertEqual(len({c["task_id"] for c in comp["missing"]}), 24)

    def test_not_run_carries_no_measurable_effect(self):
        agg = _read_json(BASELINE_AGG)
        # headline present but flagged untrustworthy
        self.assertFalse(agg["headline"]["complete"])
        for arm in (edit_toolsets.TREATMENT, edit_toolsets.BASELINE):
            s = agg["arms"][arm]
            self.assertEqual(s["observed"], 0)
            self.assertEqual(s["failure_reasons"]["missing_cell"], 24)
            self.assertEqual(s["failure_reasons"]["patch_rejected"], 0)
            # process metrics are null (no measurement), not zero
            for field in ("tool_calls", "input_tokens", "output_tokens", "wall_clock_seconds"):
                self.assertIsNone(s[field])
            # matrix fully zero-filled (all eight keys present)
            self.assertEqual(set(s["matrix"]), {
                "".join(bits) for bits in itertools.product("TF", repeat=3)
            })
            self.assertEqual(sum(s["matrix"].values()), 0)
        # no deltas emitted when an arm did not complete
        self.assertEqual(agg["deltas"], {})


class RunConfigPinsTest(unittest.TestCase):
    def setUp(self):
        self.cfg = _read_json(CONFIG)

    def test_schema_and_identity(self):
        self.assertEqual(self.cfg["schema"], "edit_run_config_v1")
        self.assertEqual(self.cfg["config_id"], "stage3-edits-baseline")
        self.assertEqual(self.cfg["status"], "pinned_not_executed")

    def test_pins_model_repetitions_and_budget(self):
        self.assertIn("intended", self.cfg["model"])
        self.assertEqual(self.cfg["repetitions"]["per_cell"], 1)
        self.assertEqual(self.cfg["budget"]["planned_total_cells"], 48)
        # a paid run requires an approved model + a spending/time budget recorded
        self.assertIsNone(self.cfg["budget"]["proposed_spending_budget_usd"])

    def test_pins_commits_tree_hashes_and_seeds(self):
        for c in self.cfg["corpora"]:
            self.assertRegex(c["source_commit"], r"^[0-9a-f]{40}$")
            self.assertRegex(c["reference_tree_hash"], r"^[0-9a-f]{64}$")
            self.assertEqual(c["seed"], 7)
        self.assertEqual(self.cfg["seeds"], [7])

    def test_pins_schema_versions(self):
        prov = self.cfg["provenance"]
        self.assertEqual(prov["harness"]["task_schema_version"], "edit_task_v1")
        self.assertEqual(prov["scorer"]["score_schema_version"], "edit_score_v1")
        self.assertEqual(prov["scorer"]["aggregate_schema_version"], "edit_aggregate_v1")
        self.assertEqual(prov["scorer"]["gate_versions_recorded"]["oracle"], "oracle_gate_v1")
        self.assertEqual(prov["scorer"]["gate_versions_recorded"]["conformance"], "conformance_gate_v2")

    def test_pins_lci_binary(self):
        b = self.cfg["subject_binary"]
        self.assertRegex(b["built_from_checkout_commit"], r"^[0-9a-f]{40}$")
        self.assertEqual(b["reported_version"], "0.10.1")

    def test_pins_per_arm_allowlists(self):
        arms = self.cfg["arms"]
        # canonical runner arms are exactly the two the config describes
        self.assertEqual(
            set(arms) - {"single_variable"},
            {edit_toolsets.TREATMENT, edit_toolsets.BASELINE},
        )
        self.assertEqual(
            arms[edit_toolsets.TREATMENT]["allowlist"], list(edit_toolsets.TREATMENT_TOOLS)
        )
        self.assertEqual(
            arms[edit_toolsets.BASELINE]["allowlist"], list(edit_toolsets.BASELINE_TOOLS)
        )
        # baseline must not be able to reach the LCI server; Bash denied both
        self.assertIn("mcp__lci__*", arms[edit_toolsets.BASELINE]["denied"])
        for arm in (edit_toolsets.TREATMENT, edit_toolsets.BASELINE):
            self.assertIn("Bash", arms[arm]["denied"])
            self.assertNotIn("Bash", arms[arm]["allowlist"])

    def test_approval_and_execution_not_granted(self):
        self.assertEqual(self.cfg["approval"]["status"], "not_granted")
        self.assertIsNone(self.cfg["approval"]["granted_by"])
        self.assertFalse(self.cfg["execution"]["executed"])
        self.assertEqual(self.cfg["execution"]["cells_planned"], 48)
        self.assertEqual(self.cfg["execution"]["cells_executed"], 0)
        self.assertEqual(self.cfg["execution"]["record_failure_status"], "missing_cell")
        self.assertEqual(self.cfg["execution"]["provider_spend_usd"], 0.0)


if __name__ == "__main__":
    unittest.main()
