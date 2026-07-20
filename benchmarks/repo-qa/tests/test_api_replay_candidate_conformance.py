import importlib.util
import hashlib
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
CANDIDATES = ROOT / "benchmarks/repo-qa/api-replay/candidates"
HARNESS = CANDIDATES / "conformance.py"
BANK = CANDIDATES / "value-bank.json"
SPEC = importlib.util.spec_from_file_location("candidate_conformance", HARNESS)
conformance = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(conformance)


class CandidateConformanceTest(unittest.TestCase):
    def candidate(self, source):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "candidate.py"
        path.write_text(source, encoding="utf-8")
        return conformance.load_candidate(path)

    def test_value_bank_is_canonical_fixed_and_covers_required_classes(self):
        bank = conformance.load_bank(BANK)
        self.assertEqual(
            hashlib.sha256(BANK.read_bytes()).hexdigest(),
            "f56258a17702fc3d10b38be6224f2f2c81e2c0d75f88098a37da9a8aab2580a1",
        )
        categories = {case["category"] for case in bank["cases"]}
        self.assertTrue({"lci_search", "lci_error", "lci_multi"} <= categories)
        self.assertTrue({"json_root", "json_keys", "json_unicode", "json_nesting"} <= categories)
        self.assertGreaterEqual(len(bank["cases"]), 16)

    def test_identity_json_codec_passes_every_case_and_reports_measurements(self):
        candidate = self.candidate(
            "import json\n"
            "def encode(value): return json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=True,allow_nan=False)\n"
            "def decode(text): return json.loads(text)\n"
        )
        bank = conformance.load_bank(BANK)
        report = conformance.evaluate(candidate, bank)
        self.assertTrue(report["summary"]["conformant"])
        self.assertEqual(report["summary"]["passed"], len(bank["cases"]))
        self.assertEqual(report["summary"]["failed"], 0)
        self.assertGreater(report["summary"]["measured_rendered_utf8_bytes"], 0)
        self.assertTrue(all("rendered_digest" in case for case in report["cases"]))

    def test_lossy_flattening_fails_pathological_cases_without_fake_sizes(self):
        candidate = self.candidate(
            "def encode(value): return str(value)\n"
            "def decode(text): return {}\n"
        )
        report = conformance.evaluate(candidate, conformance.load_bank(BANK))
        self.assertFalse(report["summary"]["conformant"])
        failed = [case for case in report["cases"] if case["status"] == "failed"]
        self.assertTrue(failed)
        self.assertTrue(all("rendered_utf8_bytes" not in case for case in failed))

    def test_gate_rejects_nondeterminism_input_mutation_and_wrong_return_type(self):
        cases = {case["id"]: case for case in conformance.load_bank(BANK)["cases"]}
        mutating = self.candidate(
            "import json\n"
            "def encode(value):\n"
            "  if isinstance(value,dict): value['injected']=True\n"
            "  return json.dumps(value)\n"
            "def decode(text): return json.loads(text)\n"
        )
        result = conformance.evaluate_case(mutating, cases["search-single-hit"])
        self.assertEqual(result["status"], "failed")
        self.assertIn("mutated", result["failure"])

        wrong_type = self.candidate("def encode(value): return value\ndef decode(text): return text\n")
        result = conformance.evaluate_case(wrong_type, cases["root-null"])
        self.assertEqual(result["failure"], "encode must return str")

        nondeterministic = self.candidate(
            "count=0\n"
            "def encode(value):\n"
            "  global count; count += 1; return str(count)\n"
            "def decode(text): return None\n"
        )
        result = conformance.evaluate_case(nondeterministic, cases["root-null"])
        self.assertIn("deterministic", result["failure"])

    def test_loader_requires_both_codec_functions(self):
        with self.assertRaisesRegex(ValueError, "decode"):
            self.candidate("def encode(value): return ''\n")


if __name__ == "__main__":
    unittest.main()
