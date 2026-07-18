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
            annotated_run = ab.run_model(str(fake), ROOT, "provider/model", "ANNOTATED", 300)
            flat_run = ab.run_model(str(fake), ROOT, "provider/model", "FLAT", 300)
        self.assertEqual(annotated_run["status"], "answered")
        self.assertEqual(flat_run["status"], "answered")
        annotated = ab.grade(ab.parse_answers(annotated_run["answer"]), fixture["expected"])
        flat = ab.grade(ab.parse_answers(flat_run["answer"]), fixture["expected"])
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
            files = ab.subprocess.run(["git", "ls-files"], cwd=workspace, text=True, capture_output=True, check=True).stdout.splitlines()
            self.assertEqual(files, [".gitignore", "opencode.json"])

    def test_full_ids_required_and_grid_has_both_capability_tiers(self):
        self.assertEqual(ab.DEFAULT_MODELS, ["opencode/deepseek-v4-flash-free", "opencode-go/glm-5.2"])
        self.assertEqual(len(ab.load_bank()), 28)

    def test_resume_key_and_atomic_result_are_stable(self):
        key = ab.cell_key("search", "annotated", "opencode-go/glm-5.2", 2)
        self.assertEqual(key, "search__annotated__opencode-go_glm-5.2__r2")
        with tempfile.TemporaryDirectory() as parent:
            path = Path(parent) / "cell.json"
            ab.write_atomic(path, {"status": "answered"})
            self.assertEqual(json.loads(path.read_text()), {"status": "answered"})

    def test_malformed_answers_fail_closed(self):
        with self.assertRaises((json.JSONDecodeError, ValueError)):
            ab.parse_answers("not json")
        with self.assertRaises(ValueError):
            ab.parse_answers('{"answers":"not-a-list"}')


if __name__ == "__main__":
    unittest.main()
