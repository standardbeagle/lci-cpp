import importlib.util, json, tempfile, unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[3]
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path); mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod); return mod
ab=load("response_shape_ab",ROOT/"benchmarks/repo-qa/scripts/response_shape_ab.py")

class ResponseShapeABTest(unittest.TestCase):
    def setUp(self): self.manifest,self.tasks=ab.load_inputs()

    def test_frozen_bank_and_matched_content(self):
        self.assertEqual(len(self.tasks),6); self.assertIn("null_control",{x["stratum"] for x in self.tasks})
        for task in self.tasks:
            self.assertEqual(ab.parse_compact(task["arms"]["shape_17"]),task["facts"])
            self.assertEqual(ab.parse_labeled(task["arms"]["shape_42"]),task["facts"])

    def test_validator_rejects_plausible_fact_and_label_drift(self):
        bank={"tasks":json.loads(json.dumps(self.tasks))}
        bank["tasks"][0]["arms"]["shape_42"]=bank["tasks"][0]["arms"]["shape_42"].replace("src/widget.cc:18","src/wrong.cc:18",1)
        with self.assertRaisesRegex(ValueError,"fact drift"): ab.validate_bank(self.manifest,bank)
        bank={"tasks":json.loads(json.dumps(self.tasks))}; bank["tasks"][0]["arms"]["shape_42"] += "\nsummary: helpful"
        with self.assertRaisesRegex(ValueError,"wrong labeled fields"): ab.validate_bank(self.manifest,bank)

    def test_oracle_discriminates_omission_invention_evidence_and_malformed(self):
        task=self.tasks[3]
        good={"answers":["timeout is 30 seconds"],"evidence":["config/runtime.toml:12"],"claims":["timeout is 30 seconds"]}
        self.assertEqual(ab.score_answer(task,good)["correct"],True)
        omitted={**good,"answers":[]}; self.assertFalse(ab.score_answer(task,omitted)["correct"])
        invented={**good,"claims":["timeout is 99 seconds"]}; self.assertTrue(ab.score_answer(task,invented)["hallucinated"])
        wrong={**good,"evidence":["config/runtime.toml:99"]}; self.assertEqual(ab.score_answer(task,wrong)["evidence_recall"],0)
        with self.assertRaises(ValueError): ab.parse_answer('{"answers":[]}')

    def test_fake_provider_is_hermetic_and_execution_scores(self):
        task=self.tasks[0]; model=self.manifest["models"][0]["id"]
        answer=json.dumps({"answers":["src/widget.cc:18"],"evidence":["src/widget.cc:18"],"claims":["src/widget.cc:18"]})
        fake=ab.FakeProvider({(task["id"],"shape_17",model):answer})
        self.assertFalse(hasattr(fake,"corpus")); self.assertFalse(hasattr(fake,"credentials"))
        with tempfile.TemporaryDirectory() as d:
            rec=ab.execute(fake,self.manifest,task,"shape_17",model,1,Path(d))
        self.assertEqual(rec["status"],"answered"); self.assertTrue(rec["score"]["correct"])

    def test_failures_unscored_and_malformed_is_explicit(self):
        task=self.tasks[0]; model=self.manifest["models"][0]["id"]
        for status in ("provider_timeout","provider_quota","provider_error","malformed_provider_stream","empty_answer","harness_error"):
            fake=ab.FakeProvider({(task["id"],"shape_17",model):{"status":status,"answer":"","failure_reason":status}})
            with tempfile.TemporaryDirectory() as d: rec=ab.execute(fake,self.manifest,task,"shape_17",model,1,Path(d))
            self.assertIsNone(rec["score"])
        fake=ab.FakeProvider({(task["id"],"shape_17",model):"not json"})
        with tempfile.TemporaryDirectory() as d: rec=ab.execute(fake,self.manifest,task,"shape_17",model,1,Path(d))
        self.assertEqual(rec["status"],"malformed_answer"); self.assertIsNone(rec["score"])

    def test_digest_key_resume_corruption_staleness_and_model_collision(self):
        task=self.tasks[0]; m1="provider/a_b"; m2="provider_a/b"
        one,_=ab.cell_identity(self.manifest,task,"shape_17",m1,1); two,_=ab.cell_identity(self.manifest,task,"shape_17",m2,1)
        self.assertNotEqual(one["cell_key"],two["cell_key"])
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"x.json"; path.write_text("bad")
            with self.assertRaises(RuntimeError): ab.reusable(path,one)
            path.write_text(json.dumps({**one,"status":"provider_timeout","answer":"","score":None})); self.assertFalse(ab.reusable(path,one))
            stale={**one,"fixture_digest":"sha256:stale"}; path.write_text(json.dumps({**stale,"status":"answered","answer":"{}","score":{}})); self.assertFalse(ab.reusable(path,one))

    def test_retry_preserves_failed_attempt(self):
        task=self.tasks[0]; model=self.manifest["models"][0]["id"]
        failure={"status":"provider_error","answer":"","failure_reason":"startup"}
        good=json.dumps({"answers":["src/widget.cc:18"],"evidence":["src/widget.cc:18"],"claims":["src/widget.cc:18"]})
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)
            ab.execute(ab.FakeProvider({(task["id"],"shape_17",model):failure}),self.manifest,task,"shape_17",model,1,out)
            rec=ab.execute(ab.FakeProvider({(task["id"],"shape_17",model):good}),self.manifest,task,"shape_17",model,1,out)
            archived=list((out/"attempts").glob("*.json"))
            self.assertEqual(len(archived),1)
            self.assertEqual(json.loads(archived[0].read_text())["status"],"provider_error")
            self.assertEqual(rec["status"],"answered")

    def test_order_is_deterministic_counterbalanced_and_grid_is_exact(self):
        jobs=ab.planned_grid(self.manifest,self.tasks); self.assertEqual(len(jobs),48)
        self.assertEqual(jobs,ab.planned_grid(self.manifest,self.tasks))
        first=[x[1] for x in jobs[::2]]; self.assertEqual(set(first),{"shape_17","shape_42"})

    def test_fake_multi_model_grid_is_complete(self):
        with tempfile.TemporaryDirectory() as d:
            records=ab.run_grid(ab.DeterministicModelProvider(),self.manifest,self.tasks,Path(d))
            self.assertEqual(len(records),48); self.assertTrue(all(x["score"]["correct"] for x in records))
            self.assertEqual({x["model"] for x in records},{x["id"] for x in self.manifest["models"]})

if __name__=="__main__": unittest.main()
