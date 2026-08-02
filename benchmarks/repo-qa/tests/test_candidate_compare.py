import importlib.util
import json
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).parents[3]
CANDIDATES = ROOT / "benchmarks/repo-qa/api-replay/candidates"
sys.path.insert(0, str(CANDIDATES))
SPEC = importlib.util.spec_from_file_location("candidate_compare", CANDIDATES / "compare.py")
compare = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(compare)


class CandidateComparisonTest(unittest.TestCase):
    def test_fixture_adds_actual_selected_tool_value(self):
        fixture = json.loads(
            (ROOT / "benchmarks/repo-qa/api-replay/fixtures/live-search-callsite.json").read_text()
        )
        values = compare.source_values({"cases": []}, fixture)
        self.assertEqual(values[0]["id"], "live:search-callsite")
        self.assertEqual(values[0]["value"]["results"][0]["hits"][0]["line"], 119)

    def test_measurement_reports_ratio_without_quality_claim(self):
        module = compare.conformance.load_candidate(CANDIDATES / "tagged_blocks_v1.py")
        result = compare.measure(module, [{"id": "x", "value": {"x": 1}}])
        self.assertGreater(result["byte_ratio"], 0)
        self.assertNotIn("better", result)
        self.assertNotIn("winner", result)

    def test_measure_shields_source_values_from_a_mutating_candidate(self):
        class MutatingCodec:
            NAME = "mutating"

            @staticmethod
            def encode(value):
                value["x"] = "corrupted"
                return json.dumps(value)

        values = [{"id": "a", "value": {"x": 1}}, {"id": "b", "value": {"x": 2}}]
        compare.measure(MutatingCodec, values)
        self.assertEqual(values[0]["value"], {"x": 1})
        self.assertEqual(values[1]["value"], {"x": 2})


if __name__ == "__main__":
    unittest.main()
