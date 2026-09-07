"""Per-model response-format comprehension scorecard (R2).

Every test here is hermetic: the multi-model provider replays committed
`opencode run --format json` recordings, so no test needs credentials, a
corpus, or a paid call.
"""
import importlib.util, json, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BASE = ROOT / "benchmarks/repo-qa/response-shape"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


ab = load("rsab_sc", ROOT / "benchmarks/repo-qa/scripts/response_shape_ab.py")
analysis = load("rsa_sc", ROOT / "benchmarks/repo-qa/scripts/analyze_response_shape_ab.py")
card = load("rssc", ROOT / "benchmarks/repo-qa/scripts/response_shape_scorecard.py")


class ModelRosterTest(unittest.TestCase):
    """The roster is configuration, and configuration that lies is a defect."""

    def test_committed_rosters_load_and_declare_tiers_and_probes(self):
        for path in (BASE / "models.json", BASE / "models.fake.json"):
            roster = card.load_models(path)
            self.assertGreaterEqual(len(roster["models"]), 2)
            for model in roster["models"]:
                self.assertIn("/", model["id"])            # full provider/model id
                self.assertIn(model["tier"], roster["classes"])
                self.assertTrue(model["id_probe"].strip())  # rule 10b: cite the probe

    def test_real_roster_holds_the_two_verified_free_models(self):
        ids = [m["id"] for m in card.load_models(BASE / "models.json")["models"]]
        self.assertEqual(ids, ["opencode-go/deepseek-v4-flash", "opencode-go/glm-5.2"])

    def test_fake_roster_spans_at_least_three_models_over_two_classes(self):
        roster = card.load_models(BASE / "models.fake.json")
        self.assertGreaterEqual(len(roster["models"]), 3)
        self.assertGreaterEqual(len({m["tier"] for m in roster["models"]}), 2)

    def test_roster_rejects_duplicates_bare_ids_unknown_tiers_and_unprobed_ids(self):
        good = json.loads((BASE / "models.fake.json").read_text())
        for mutate, message in (
            (lambda r: r["models"].append(dict(r["models"][0])), "duplicate"),
            (lambda r: r["models"][0].update(id="bare-model"), "provider/model"),
            (lambda r: r["models"][0].update(tier="galaxy"), "tier"),
            (lambda r: r["models"][0].update(id_probe=""), "probe"),
        ):
            roster = json.loads(json.dumps(good))
            mutate(roster)
            with tempfile.TemporaryDirectory() as d:
                path = Path(d) / "models.json"
                path.write_text(json.dumps(roster))
                with self.assertRaisesRegex(ValueError, message):
                    card.load_models(path)


class DigestCompatibilityTest(unittest.TestCase):
    """R1 ledger records must stay reusable: same identity fields, plus model."""

    def test_scorecard_cells_are_identical_to_r1_cells_for_a_shared_model(self):
        manifest, tasks = ab.load_inputs()
        model = manifest["models"][0]["id"]
        r1, _ = ab.cell_identity(manifest, tasks[0], "shape_17", model, 1)
        roster = card.load_models(BASE / "models.json")
        r2, _ = ab.cell_identity(manifest, tasks[0], "shape_17", roster["models"][0]["id"], 1)
        self.assertEqual(r1["cell_key"], r2["cell_key"])
        self.assertEqual(r1["manifest_digest"], r2["manifest_digest"])

    def test_roster_drives_the_grid_without_touching_the_frozen_manifest(self):
        manifest, tasks = ab.load_inputs()
        roster = card.load_models(BASE / "models.fake.json")
        jobs = ab.planned_grid(manifest, tasks, models=[m["id"] for m in roster["models"]])
        self.assertEqual(len(jobs), len(tasks) * len(roster["models"]) * manifest["repetitions"] * 2)
        self.assertEqual({j[2] for j in jobs}, {m["id"] for m in roster["models"]})


class FakeMultiModelProviderTest(unittest.TestCase):
    def test_streams_are_verbatim_recorded_envelopes_parsed_by_the_live_parser(self):
        roster = card.load_models(BASE / "models.fake.json")
        provider = card.FakeMultiModelProvider(roster)
        manifest, tasks = ab.load_inputs()
        for model in roster["models"]:
            recorded = (BASE / "recordings" / model["recording"]).read_text().splitlines()
            lines = provider.stream(tasks[3], "shape_42", model["id"])
            self.assertEqual(len(lines), len(recorded))
            envelope = [l for l in lines if '"type":"text"' not in l]
            self.assertEqual(envelope, [l for l in recorded if '"type":"text"' not in l])
            out = provider.run(prompt="p", task=tasks[3], arm="shape_42",
                               model=model["id"], timeout=1)
            self.assertEqual(out["status"], "answered")
            self.assertEqual(out["recording"], model["recording"])
            # real recorded token block, including the cache sub-object
            self.assertGreater(out["tokens"]["input"], 0)
            self.assertIn("cache_read", out["tokens"])

    def test_the_grid_is_hermetic_end_to_end_and_discriminates_model_behaviour(self):
        report = card.plan_run_report_roster(BASE / "models.fake.json", out=None)["report"]
        self.assertTrue(report["complete"])
        self.assertTrue(report["accounting"]["reconciled"])
        by_model = {m: arms["shape_42"]["correctness"] for m, arms in report["models"].items()}
        self.assertEqual(len(by_model), 3)
        # a faithful model, a terse one (omits an atom), a wrapped one -- the
        # scorecard must separate them, not average them into one number
        self.assertGreater(max(by_model.values()), min(by_model.values()))


class AccountingAndRollupTest(unittest.TestCase):
    def setUp(self):
        self.manifest, self.tasks = ab.load_inputs()
        self.models = [m["id"] for m in self.manifest["models"]]
        self.records = []
        for task, arm, model, rep in ab.planned_grid(self.manifest, self.tasks, models=self.models):
            identity, _ = ab.cell_identity(self.manifest, task, arm, model, rep)
            self.records.append({**identity, "status": "answered", "wall_seconds": 0.1,
                                 "tokens": {"input": 10, "output": 4, "reasoning": 2,
                                            "cache_read": 7, "cache_write": 3},
                                 "score": {"correct": True, "hallucinated": False,
                                           "evidence_recall": 1.0, "answer_recall": 1.0,
                                           "omission_count": 0}})

    def test_accounting_reconciles_planned_recorded_graded_and_failed(self):
        report = analysis.analyze(self.manifest, self.tasks, self.records, models=self.models)
        acct = report["accounting"]
        self.assertEqual(acct["planned"], len(self.records))
        self.assertEqual(acct["recorded"] + acct["missing"], acct["planned"])
        self.assertEqual(acct["graded"] + acct["failed"], acct["recorded"])
        self.assertTrue(acct["reconciled"])

    def test_a_missing_cell_breaks_accounting_and_refuses_a_scorecard(self):
        report = analysis.analyze(self.manifest, self.tasks, self.records[:-1], models=self.models)
        self.assertFalse(report["complete"])
        self.assertEqual(report["accounting"]["missing"], 1)
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaisesRegex(ValueError, "incomplete"):
                analysis.write_scorecards(Path(d), report)

    def test_a_rollup_that_would_hide_a_model_fails(self):
        report = analysis.analyze(self.manifest, self.tasks, self.records, models=self.models)
        for key, rollup in report["class_rollups"].items():
            self.assertTrue(rollup["members"])
        covered = {m for r in report["class_rollups"].values() for m in r["members"]}
        self.assertEqual(covered, set(self.models))
        hidden = json.loads(json.dumps(report))
        for rollup in hidden["class_rollups"].values():
            rollup["members"] = rollup["members"][:-1]
        with self.assertRaisesRegex(ValueError, "hides"):
            analysis.verify_rollup_coverage(hidden)

    def test_a_mixed_schema_record_is_refused(self):
        mixed = json.loads(json.dumps(self.records))
        mixed[0]["grading_schema"] = "atomic-value-v2"
        with self.assertRaisesRegex(ValueError, "grading_schema"):
            analysis.analyze(self.manifest, self.tasks, mixed, models=self.models)
        mixed = json.loads(json.dumps(self.records))
        mixed[0]["analysis_revision"] = "paired-task-v1"
        with self.assertRaisesRegex(ValueError, "analysis_revision"):
            analysis.analyze(self.manifest, self.tasks, mixed, models=self.models)

    def test_cache_tokens_are_reported_separately_and_never_summed(self):
        report = analysis.analyze(self.manifest, self.tasks, self.records, models=self.models)
        cell = next(iter(report["cells"].values()))
        self.assertEqual(cell["input_tokens"], 10.0)
        self.assertEqual(cell["output_tokens"], 4.0)
        self.assertEqual(cell["reasoning_tokens"], 2.0)
        self.assertEqual(cell["cache_read_tokens"], 7.0)
        self.assertEqual(cell["cache_write_tokens"], 3.0)
        text = analysis.markdown(report)
        self.assertIn("cache read/write", text)


class RealProviderGuardTest(unittest.TestCase):
    def test_real_provider_run_without_the_explicit_guard_refuses(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit):
                card.main(["run", "--models", str(BASE / "models.json"),
                           "--out", d, "--real-provider"])
            self.assertEqual(list(Path(d).glob("**/*.json")), [])


class CommittedScorecardTest(unittest.TestCase):
    """Rule 8: a committed artefact that describes a run is checked against it."""

    def test_committed_fake_scorecard_matches_a_fresh_fake_run(self):
        committed = json.loads((BASE / "scorecards/fake/analysis.json").read_text())
        fresh = card.plan_run_report_roster(BASE / "models.fake.json", out=None)["report"]
        self.assertEqual(committed["models"], fresh["models"])
        self.assertEqual(committed["class_rollups"], fresh["class_rollups"])
        self.assertEqual(committed["accounting"], fresh["accounting"])
        self.assertEqual((BASE / "scorecards/fake/report.md").read_text(), analysis.markdown(fresh))


if __name__ == "__main__":
    unittest.main()
