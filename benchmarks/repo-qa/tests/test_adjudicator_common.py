import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/adjudicator_common.py"
SPEC = importlib.util.spec_from_file_location("adjudicator_common", SCRIPT)
common = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(common)


class ModelSuffixTest(unittest.TestCase):
    def test_extracts_provider_local_model_id(self):
        self.assertEqual(common.model_suffix("opencode-go/glm-5.2"), "glm-5.2")
        self.assertEqual(common.model_suffix("opencode/a/b"), "a/b")

    def test_bare_model_id_fails_with_a_named_error(self):
        for bad in ("glm-5.2", "opencode/", "/glm-5.2", ""):
            with self.assertRaisesRegex(ValueError, "provider/model"):
                common.model_suffix(bad)


class StripCodeFenceTest(unittest.TestCase):
    def test_plain_and_fenced_json_pass_through(self):
        self.assertEqual(common.strip_code_fence('{"a":1}'), '{"a":1}')
        self.assertEqual(common.strip_code_fence('```json\n{"a":1}\n```'), '{"a":1}')

    def test_missing_closing_fence_keeps_the_final_json_line(self):
        self.assertEqual(common.strip_code_fence('```json\n{"a":1}'), '{"a":1}')

    def test_trailing_prose_after_the_fence_is_dropped(self):
        text = '```json\n{"a":1}\n```\nHope this helps!'
        self.assertEqual(common.strip_code_fence(text), '{"a":1}')


class RequestBodyTest(unittest.TestCase):
    def test_body_is_deterministic_and_unstreamed(self):
        body = common.request_body("opencode-go/glm-5.2", "prompt", system="judge")
        self.assertEqual(body["model"], "glm-5.2")
        self.assertEqual(body["temperature"], 0)
        self.assertFalse(body["stream"])
        self.assertEqual(body["messages"][0], {"role": "system", "content": "judge"})
        self.assertEqual(body["messages"][1], {"role": "user", "content": "prompt"})


class CheckpointTest(unittest.TestCase):
    def test_checkpoint_is_atomic_with_temp_cleanup_on_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.json"
            common.write_checkpoint(path, {"records": []})
            self.assertEqual(json.loads(path.read_text()), {"records": []})
            with mock.patch.object(common.replay_common.os, "replace",
                                   side_effect=RuntimeError("boom")):
                with self.assertRaises(RuntimeError):
                    common.write_checkpoint(path, {"records": [1]})
            self.assertEqual([p.name for p in Path(directory).iterdir()], ["out.json"])
            self.assertEqual(json.loads(path.read_text()), {"records": []})


class HeaderProfileTest(unittest.TestCase):
    def test_header_profile_requires_the_committed_headers_key(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            path.write_text(json.dumps({"headers": {"x-a": "b"}}))
            self.assertEqual(common.load_header_profile(path), {"x-a": "b"})
            path.write_text(json.dumps({"request_headers": {"x-a": "b"}}))
            with self.assertRaisesRegex(SystemExit, "must carry a 'headers' object"):
                common.load_header_profile(path)


if __name__ == "__main__":
    unittest.main()
