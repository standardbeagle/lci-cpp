import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/capture_lci_tool_matrix.py"
SPEC = importlib.util.spec_from_file_location("capture_lci_tool_matrix", SCRIPT)
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class FakeSession:
    def rpc(self, method, params=None):
        if method == "tools/list":
            return {"tools": [{"name": name} for name in module.NEGATIVE_ARGUMENTS]}
        return {"content": [{"type": "text", "text": params["name"] + " output"}],
                "isError": params["arguments"] == module.NEGATIVE_ARGUMENTS[params["name"]]}


class CaptureLciToolMatrixTest(unittest.TestCase):
    def test_captures_two_genuine_calls_for_every_live_tool(self):
        successes = {name: {"success": name} for name in module.NEGATIVE_ARGUMENTS}
        result = module.capture(FakeSession(), successes)
        self.assertEqual(result["tool_count"], len(module.NEGATIVE_ARGUMENTS))
        self.assertEqual(result["case_count"], 2 * len(module.NEGATIVE_ARGUMENTS))
        self.assertEqual({item["scenario"] for item in result["cases"]}, {"success", "negative"})

    def test_text_content_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "no text"):
            module.text_content({"content": [{"type": "image", "data": "x"}]})


if __name__ == "__main__":
    unittest.main()
