import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/tool_claim_oracle.py"
SPEC = importlib.util.spec_from_file_location("tool_claim_oracle", SCRIPT)
oracle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(oracle)


class ToolClaimOracleTest(unittest.TestCase):
    def test_generated_bank_covers_and_discriminates_every_case(self):
        matrix = json.loads((ROOT / "benchmarks/repo-qa/api-replay/all-tools/tool-output-matrix.json").read_text())
        bank = json.loads((ROOT / "benchmarks/repo-qa/api-replay/oracle-v2/claim-schemas.json").read_text())
        oracle.validate_bank(bank, matrix)

    def test_location_accepts_prose_but_not_wrong_line(self):
        schema = {"task_id": "x", "required_claims": [
            {"id": "site", "kind": "location", "value": "pocketbase.go:166"}]}
        self.assertTrue(oracle.evaluate("pocketbase.go at line 166", schema)["exact"])
        result = oracle.evaluate("pocketbase.go at line 167", schema)
        self.assertFalse(result["exact"])
        self.assertEqual(result["failed_claims"], ["site"])

    def test_fallback_rejects_no_data(self):
        claim = {"id": "outcome", "kind": "outcome", "value": "fallback"}
        self.assertFalse(oracle.evaluate_claim("It returned no data.", claim)[0])

    def test_stale_source_digest_is_rejected(self):
        matrix = json.loads((ROOT / "benchmarks/repo-qa/api-replay/all-tools/tool-output-matrix.json").read_text())
        bank = json.loads((ROOT / "benchmarks/repo-qa/api-replay/oracle-v2/claim-schemas.json").read_text())
        bank["schemas"][0]["source_output_digest"] = oracle.digest("stale")
        with self.assertRaisesRegex(ValueError, "source output digest mismatch"):
            oracle.validate_bank(bank, matrix)


if __name__ == "__main__":
    unittest.main()
