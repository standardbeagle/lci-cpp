import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SURFACE = ROOT / "benchmarks/repo-qa/comprehension/surface/tool-surface.json"
VARIANTS = ROOT / "benchmarks/repo-qa/comprehension/formats/variants.json"


class FormatVariantsTest(unittest.TestCase):
    def setUp(self):
        self.surface = json.loads(SURFACE.read_text())
        self.bank = json.loads(VARIANTS.read_text())

    def test_every_live_tool_has_current_and_annotated_plain_text(self):
        live = {tool["name"] for tool in self.surface["tools"]}
        bank = {tool["name"] for tool in self.bank["tools"]}
        self.assertEqual(live, bank)
        for tool in self.bank["tools"]:
            variants = {variant["id"]: variant for variant in tool["variants"]}
            self.assertIn("current", variants)
            self.assertIn("annotated", variants)
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

    def test_controls_receive_both_formats(self):
        surface_controls = {t["name"] for t in self.surface["tools"] if t.get("control")}
        bank_controls = {t["name"] for t in self.bank["tools"] if t.get("control")}
        self.assertEqual(surface_controls, bank_controls)

    def test_serialization_is_byte_stable(self):
        first = json.dumps(self.bank, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
        second = json.dumps(json.loads(first), indent=2, sort_keys=True, ensure_ascii=False) + "\n"
        self.assertEqual(first, second)


if __name__ == "__main__":
    unittest.main()
