import json
import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SURFACE = ROOT / "benchmarks/repo-qa/comprehension/surface/tool-surface.json"
VARIANTS = ROOT / "benchmarks/repo-qa/comprehension/formats/variants.json"
CAPTURES = ROOT / "benchmarks/repo-qa/comprehension/formats/live-captures.json"
CAPTURE_SCRIPT = ROOT / "benchmarks/repo-qa/comprehension/formats/capture_variants.py"
SPEC = importlib.util.spec_from_file_location("capture_variants", CAPTURE_SCRIPT)
capture = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(capture)


class FormatVariantsTest(unittest.TestCase):
    def setUp(self):
        self.surface = json.loads(SURFACE.read_text())
        self.bank = json.loads(VARIANTS.read_text())
        self.captures = json.loads(CAPTURES.read_text())

    def test_every_live_tool_has_current_and_annotated_plain_text(self):
        live = {tool["name"] for tool in self.surface["tools"]}
        names = [tool["name"] for tool in self.bank["tools"]]
        bank = set(names)
        self.assertEqual(live, bank)
        self.assertEqual(len(names), len(bank), "duplicate tool entries")
        for tool in self.bank["tools"]:
            ids = [variant["id"] for variant in tool["variants"]]
            self.assertEqual(ids, ["current", "annotated"])
            variants = {variant["id"]: variant for variant in tool["variants"]}
            for variant in variants.values():
                self.assertIsInstance(variant["text"], str)
                self.assertTrue(variant["text"].strip())
                self.assertIsInstance(variant["production_faithful"], bool)
                if not variant["production_faithful"]:
                    self.assertTrue(variant.get("boundary_note"))

    def test_stated_answers_exactly_match_independent_oracle(self):
        expected = {tool["name"]: tool["answer"] for tool in self.surface["tools"]}
        actual = {tool["name"]: tool["answer"] for tool in self.bank["tools"]}
        self.assertEqual(expected, actual)

    def test_production_faithful_variants_carry_every_answer(self):
        for tool in self.bank["tools"]:
            for variant in tool["variants"]:
                if variant["production_faithful"]:
                    for answer in tool["answer"]:
                        if ":" in answer:
                            path, line = answer.rsplit(":", 1)
                            self.assertIn(path, variant["text"])
                            self.assertIn(line, variant["text"])
                        else:
                            self.assertIn(answer, variant["text"])

    def test_current_blobs_equal_normalized_live_captures(self):
        records = self.captures["captures"]
        self.assertEqual(len(records), len({record["name"] for record in records}))
        captured = {record["name"]: record["normalized_output"] for record in records}
        self.assertEqual(set(captured), {tool["name"] for tool in self.bank["tools"]})
        for tool in self.bank["tools"]:
            current = tool["variants"][0]
            self.assertEqual(current["text"], captured[tool["name"]])

    def test_controls_receive_both_formats(self):
        surface_controls = {t["name"] for t in self.surface["tools"] if t.get("control")}
        bank_controls = {t["name"] for t in self.bank["tools"] if t.get("control")}
        self.assertEqual(surface_controls, bank_controls)

    def test_committed_json_is_canonical_and_byte_stable(self):
        for path in (VARIANTS, CAPTURES):
            value = json.loads(path.read_text())
            encoded = json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
            self.assertEqual(path.read_text(), encoded)

    def test_normalization_removes_all_runtime_timings(self):
        first = '{"performance":{"total_time_ms":2,"component_breakdown":{"ai_time":1}},"count":3}'
        second = '{"performance":{"total_time_ms":999,"component_breakdown":{"ai_time":88}},"count":3}'
        self.assertEqual(capture.normalize_text(first, ROOT), capture.normalize_text(second, ROOT))
        self.assertEqual(capture.normalize_text(first, ROOT), '{"count":3,"performance":{"component_breakdown":{}}}')


if __name__ == "__main__":
    unittest.main()
