import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/comprehension_ab.py"
FIXTURE = ROOT / "benchmarks/repo-qa/comprehension/runner/fileblob-close.json"
SPEC = importlib.util.spec_from_file_location("comprehension_ab", SCRIPT)
ab = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(ab)


class ComprehensionAbTest(unittest.TestCase):
    def test_known_fileblob_split_discriminates_harness(self):
        fixture = json.loads(FIXTURE.read_text())
        with tempfile.TemporaryDirectory() as parent:
            fake = Path(parent) / "fake-opencode"
            fake.write_text(
                "#!/usr/bin/env python3\n"
                "import json,sys\n"
                f"answer = {fixture['flat_weak_answer']!r} if 'FLAT' in sys.argv[-1] else {fixture['annotated_answer']!r}\n"
                "print(json.dumps({'part': {'type': 'text', 'messageID': 'm1', 'text': answer}}))\n"
            )
            fake.chmod(0o755)
            base = {"tool": "fileblob_close", "question": "find callers", "expected": fixture["expected"], "production_faithful": True}
            annotated_cell = {**base, "variant": "annotated", "blob": "ANNOTATED"}
            flat_cell = {**base, "variant": "flat", "blob": "FLAT"}
            annotated_run = ab.execute_cell(str(fake), ROOT, annotated_cell, "provider/model", 1, 300, Path(parent) / "annotated.json")
            flat_run = ab.execute_cell(str(fake), ROOT, flat_cell, "provider/model", 1, 300, Path(parent) / "flat.json")
            self.assertTrue((Path(parent) / "annotated.json").exists())
            self.assertTrue((Path(parent) / "flat.json").exists())
        self.assertEqual(annotated_run["status"], "answered")
        self.assertEqual(flat_run["status"], "answered")
        annotated = annotated_run["score"]
        flat = flat_run["score"]
        self.assertEqual((annotated["precision"], annotated["recall"], annotated["false_positive_count"]), (1.0, 1.0, 0))
        self.assertEqual((flat["precision"], flat["recall"], flat["false_positive_count"]), (0.75, 1.0, 2))

    def test_provider_failures_are_unscored(self):
        with mock.patch.object(ab.subprocess, "run", side_effect=ab.subprocess.TimeoutExpired("opencode", 300)):
            result = ab.run_model("opencode", ROOT, "provider/model", "prompt", 300)
        self.assertEqual(result["status"], "provider_timeout")

    def test_empty_git_workspace_has_commit_and_no_corpus(self):
        with tempfile.TemporaryDirectory() as parent:
            workspace = ab.empty_git_workspace(Path(parent))
            self.assertTrue((workspace / ".git").is_dir())
            config = json.loads((workspace / "opencode.json").read_text())
            self.assertEqual(config["tools"], {"*": False})
            self.assertEqual(config["permission"], {"*": "deny"})
            files = ab.subprocess.run(["git", "ls-files"], cwd=workspace, text=True, capture_output=True, check=True).stdout.splitlines()
            self.assertEqual(files, [".gitignore", "opencode.json"])

    def test_full_ids_required_and_grid_has_both_capability_tiers(self):
        self.assertEqual(ab.DEFAULT_MODELS, ["opencode/deepseek-v4-flash-free", "opencode-go/glm-5.2"])
        self.assertEqual(len(ab.load_bank()), 28)

    def test_resume_key_and_atomic_result_are_stable(self):
        key = ab.cell_key("search", "annotated", "opencode-go/glm-5.2", 2)
        self.assertRegex(key, r"^search__annotated__opencode-go_glm-5\.2-[0-9a-f]{10}__r2$")
        self.assertNotEqual(
            ab.cell_key("search", "annotated", "p/a_b", 1),
            ab.cell_key("search", "annotated", "p_a/b", 1),
        )
        with tempfile.TemporaryDirectory() as parent:
            path = Path(parent) / "cell.json"
            ab.write_atomic(path, {"status": "answered"})
            self.assertEqual(json.loads(path.read_text()), {"status": "answered"})

    def test_malformed_answers_fail_closed(self):
        with self.assertRaises((json.JSONDecodeError, ValueError)):
            ab.parse_answers("not json")
        with self.assertRaises(ValueError):
            ab.parse_answers('{"answers":"not-a-list"}')
        for malformed in ("[]", "null", "1"):
            with self.assertRaises(ValueError):
                ab.parse_answers(malformed)

    def test_quota_classification_and_timeout_floor_contract(self):
        self.assertEqual(ab.classify_failure("HTTP 429 rate limit exceeded"), "provider_quota")
        self.assertIsNone(ab.classify_failure("ordinary provider failure"))

    def test_resume_rejects_corrupt_stale_and_retryable_results(self):
        identity = {"schema": "lci.comprehension.cell.v1", "prompt_digest": "new"}
        with tempfile.TemporaryDirectory() as parent:
            path = Path(parent) / "cell.json"
            path.write_text("not json")
            with self.assertRaises(RuntimeError):
                ab.reusable_result(path, identity)
            path.write_text(json.dumps({**identity, "status": "provider_quota"}))
            self.assertFalse(ab.reusable_result(path, identity))
            path.write_text(json.dumps({**identity, "status": "answered"}))
            self.assertFalse(ab.reusable_result(path, identity))
            complete = {
                **identity, "status": "answered", "answer": '{"answers":[]}',
                "extracted": [], "score": ab.grade([], []),
            }
            path.write_text(json.dumps(complete))
            self.assertTrue(ab.reusable_result(path, identity))
            self.assertFalse(ab.reusable_result(path, {**identity, "prompt_digest": "changed"}))

    def test_malformed_provider_events_are_unscored_not_crashes(self):
        malformed = [
            json.dumps({"part": {"type": "text", "messageID": [], "text": "x"}}),
            json.dumps({"part": {"type": "text", "messageID": "m", "text": 1}}),
            json.dumps({"part": {"type": "step_finish", "tokens": {"input": "many"}}}),
        ]
        answer, metadata = ab.parse_events(malformed)
        self.assertEqual(answer, "")
        self.assertTrue(metadata["malformed_event"])

    def test_input_digest_changes_when_oracle_changes(self):
        cell = {"tool": "x", "variant": "current", "question": "q", "blob": "b", "expected": ["a"], "production_faithful": True}
        prompt = ab.PROMPT.format(question="q", blob="b")
        first = ab.hashlib.sha256(ab.canonical({"grading_schema": "precision-recall-fp.v1", "question": "q", "blob": "b", "expected": ["a"], "production_faithful": True}).encode()).hexdigest()
        second = ab.hashlib.sha256(ab.canonical({"grading_schema": "precision-recall-fp.v1", "question": "q", "blob": "b", "expected": ["changed"], "production_faithful": True}).encode()).hexdigest()
        self.assertNotEqual(first, second)


if __name__ == "__main__":
    unittest.main()
