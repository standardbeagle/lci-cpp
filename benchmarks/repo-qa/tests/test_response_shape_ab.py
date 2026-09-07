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

    def _fake_opencode(self, directory, exit_code, stderr_text):
        script=Path(directory)/"fake-opencode"
        script.write_text("#!/usr/bin/env python3\nimport sys\n"
                          f"sys.stderr.write({stderr_text!r})\nraise SystemExit({exit_code})\n")
        script.chmod(0o755)
        return script

    def _run_fake(self, exit_code, stderr_text):
        task=self.tasks[0]; manifest=dict(self.manifest,timeout_seconds=60)
        with tempfile.TemporaryDirectory() as d:
            provider=ab.OpenCodeProvider(str(self._fake_opencode(d,exit_code,stderr_text)))
            return provider.run(prompt="p",task=task,arm="shape_17",model="provider/model",
                                timeout=manifest["timeout_seconds"])

    def test_nonzero_exit_taxonomy_matches_comprehension_ab(self):
        self.assertEqual(self._run_fake(124,"")["status"],"provider_timeout")
        self.assertEqual(self._run_fake(3,"")["status"],"exit_3")
        self.assertEqual(self._run_fake(0,"")["status"],"empty_answer")

    def test_raw_provider_stderr_is_not_persisted_verbatim(self):
        noise="lead-marker\n"+"x"*4000
        record=self._run_fake(3,noise)
        self.assertNotIn("raw_provider_stderr",record)
        self.assertLessEqual(len(record["provider_stderr_tail"]),ab.STDERR_TAIL_LIMIT)
        self.assertNotIn("lead-marker",json.dumps(record))

    # --- gap closures (R1) -------------------------------------------------

    def test_manifest_names_only_live_verified_model_ids(self):
        """The retired free-tier id is not a model; `opencode models` is the oracle.

        Verified 2026-09-06 against `opencode models`: `opencode-go/deepseek-v4-flash`
        and `opencode-go/glm-5.2` are listed, `opencode/deepseek-v4-flash-free` is not.
        """
        ids={m["id"] for m in self.manifest["models"]}
        self.assertNotIn("opencode/deepseek-v4-flash-free",ids)
        self.assertEqual(ids,{"opencode-go/deepseek-v4-flash","opencode-go/glm-5.2"})
        self.assertEqual({m["tier"] for m in self.manifest["models"]},{"weak","strong"})

    def test_recorded_real_stream_replays_into_a_scored_cell(self):
        """The hermetic provider is shaped by a REAL recorded opencode stream.

        Recording: benchmarks/repo-qa/response-shape/recordings/
        opencode-go-deepseek-v4-flash.file-lines.shape_17.jsonl — captured
        2026-09-06 from `opencode run --format json -m opencode-go/deepseek-v4-flash`
        on the `file-lines` task, arm shape_17, in the tool-denied empty workspace.
        A hand-written stream would not pin the hyphenated part types or the
        nested token block the parser must survive.
        """
        recording=ROOT/"benchmarks/repo-qa/response-shape/recordings/opencode-go-deepseek-v4-flash.file-lines.shape_17.jsonl"
        lines=recording.read_text().splitlines()
        self.assertTrue(any('"step-finish"' in x for x in lines),"recording must retain real hyphenated part types")
        task=[t for t in self.tasks if t["id"]=="file-lines"][0]
        provider=ab.RecordedStreamProvider(recording)
        with tempfile.TemporaryDirectory() as d:
            rec=ab.execute(provider,self.manifest,task,"shape_17","opencode-go/deepseek-v4-flash",1,Path(d))
        self.assertEqual(rec["status"],"answered")
        self.assertTrue(rec["score"]["correct"])
        self.assertGreater(rec["tokens"]["input"],0)

    def test_provider_workspace_denies_tools_and_carries_no_corpus(self):
        with tempfile.TemporaryDirectory() as d:
            workspace=ab.empty_git_workspace(Path(d))
            config=json.loads((workspace/"opencode.json").read_text())
            self.assertEqual(config["tools"],{"*":False})
            self.assertEqual(config["permission"],{"*":"deny"})
            self.assertEqual(config["mcp"],{})
            tracked={x.name for x in workspace.iterdir() if x.name!=".git"}
            self.assertEqual(tracked,{".gitignore","opencode.json"})

    def test_token_budget_parity_is_enforced_with_the_frozen_tolerance(self):
        bank={"tasks":json.loads(json.dumps(self.tasks))}
        task=bank["tasks"][0]
        task["arms"]["shape_42"]=task["arms"]["shape_42"]+"\n"+"\n".join("" for _ in range(0))
        # neutral padding beyond max(10 tokens, 10% of arm A) must be rejected
        task["arms"]["shape_42"]=task["arms"]["shape_42"].replace(
            "next_action:","next_action: "+" ".join(["-"]*40)+" ;",1)
        with self.assertRaisesRegex(ValueError,"token drift|fact drift"):
            ab.validate_bank(self.manifest,bank)
        # the committed bank itself is inside tolerance
        ab.validate_bank(self.manifest,{"tasks":self.tasks})

    def test_one_command_plan_run_report(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)
            result=ab.plan_run_report(ab.DeterministicModelProvider(),self.manifest,self.tasks,out)
            self.assertEqual(result["cells"],48)
            self.assertTrue(result["report"]["complete"])
            self.assertTrue((out/"analysis.json").exists())
            self.assertTrue((out/"report.md").exists())
            self.assertIn("Response-format comprehension scorecard",(out/"report.md").read_text())

if __name__=="__main__": unittest.main()
