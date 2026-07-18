"""Hermetic contract tests for deterministic claim-validation scoring."""

import os
import sys
import unittest

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "exploration")
sys.path.insert(0, ROOT)

from scoring import (IncompatibleRuns, aggregate_claim_scores, parse_claim_answer,
                     score_claim_run)  # noqa: E402


def task(verdict="false", category="false-premise", classes=None):
    return {"id": "claim-one", "author": {"verdict": verdict, "category": category,
            "anchor_classification": classes or ["authoritative-live", "authoritative-live"]},
            "evidence": [{"path": "src/live.py", "lines": [3]},
                         {"path": "src/other.py", "lines": [5, 8]}]}


def record(answer, arm="treatment", **updates):
    rec = {"run_key": f"claim-one::{arm}::seed-1", "seed": 1, "arm": arm,
           "status": "answered", "structured_answer": answer, "model": "m",
           "mode": "claim-validation", "schema_version": "claim_validation_answer_v1",
           "forge_version": "forge-1", "settings": {"temperature": 0},
           "tool_calls": [], "token_usage": {}, "duration_seconds": 1}
    rec.update(updates)
    return rec


def answer(verdict="false", evidence=None):
    return {"verdict": verdict, "evidence": evidence or [{"path": "src/live.py", "line": 3}],
            "rationale": "bounded evidence"}


class ParseTests(unittest.TestCase):
    def test_json_and_dict_normalize_and_dedupe(self):
        parsed = parse_claim_answer('{"verdict":"true","evidence":['
            '{"path":"./src\\\\live.py","line":3},{"path":"src/live.py","line":3}],'
            '"rationale":"ok"}')
        self.assertEqual(parsed["evidence"], [("src/live.py", 3, 3)])

    def test_malformed_ambiguous_and_unsafe_are_invalid(self):
        bad = ["true", {"verdict": "yes", "evidence": [], "rationale": "x"},
               {"verdict": "true", "evidence": [{"path": "../x.py", "line": 1}], "rationale": "x"},
               {"verdict": "true", "evidence": [{"path": "x.py", "line": True}], "rationale": "x"}]
        self.assertTrue(all(parse_claim_answer(value) is None for value in bad))


class ScoringTests(unittest.TestCase):
    def test_correct_verdict_requires_grounding(self):
        good = score_claim_run(task(), record(answer()), task_bank_digest="bank")
        uncited = score_claim_run(task(), record(answer(evidence=[{"path": "unknown.py", "line": 3}])),
                                  task_bank_digest="bank")
        wrong = score_claim_run(task(), record(answer("true")), task_bank_digest="bank")
        self.assertTrue(good["success"])
        self.assertFalse(uncited["success"])
        self.assertEqual(uncited["evidence"]["precision"], 0.0)
        self.assertFalse(wrong["success"])

    def test_configured_evidence_rule_is_enforced(self):
        one_of_two = score_claim_run(task(), record(answer()), task_bank_digest="bank",
                                     settings={"evidence_min_recall": 1.0})
        both = answer(evidence=[{"path": "src/live.py", "line": 3},
                                {"path": "src/other.py", "line": 6}])
        complete = score_claim_run(task(), record(both), task_bank_digest="bank",
                                   settings={"evidence_min_recall": 1.0})
        self.assertFalse(one_of_two["success"])
        self.assertTrue(complete["success"])

    def test_stale_line_duplicate_and_dead_only_do_not_score(self):
        mixed = answer(evidence=[{"path": "src/live.py", "line": 3},
                                 {"path": "src/live.py", "line": 3},
                                 {"path": "src/other.py", "line": 99}])
        score = score_claim_run(task(), record(mixed), task_bank_digest="bank")
        self.assertEqual(score["evidence"]["cited_total"], 2)
        self.assertEqual(score["evidence"]["precision"], .5)
        dead = score_claim_run(task(classes=["dead", "misleading"]), record(answer()),
                               task_bank_digest="bank")
        self.assertFalse(dead["grounded"])
        self.assertEqual(dead["evidence"]["f1"], 0.0)

    def test_failures_are_preserved_not_zero_answers(self):
        failed = score_claim_run(task(), record(None, status="timeout"), task_bank_digest="bank")
        self.assertFalse(failed["valid_answer"])
        self.assertIsNone(failed["evidence"]["precision"])


class AggregateTests(unittest.TestCase):
    def _score(self, arm, verdict="false", **updates):
        rec = record(answer(verdict), arm=arm, **updates)
        return score_claim_run(task(), rec, task_bank_digest="bank")

    def test_reports_accuracy_citations_rates_confusion_and_pairing(self):
        agg = aggregate_claim_scores([self._score("treatment"), self._score("baseline", "true")])
        self.assertEqual(agg["pairing"]["paired_count"], 1)
        self.assertEqual(agg["arms"]["treatment"]["grounded_accuracy"], 1.0)
        self.assertEqual(agg["arms"]["baseline"]["verdict_accuracy"], 0.0)
        self.assertEqual(agg["arms"]["treatment"]["false_premise_rate"], 1.0)
        self.assertIn("confusion", agg["arms"]["treatment"]["categories"]["false-premise"])
        self.assertIn("citation_f1", agg["deltas"])

    def test_latest_run_key_wins_and_unpaired_is_forensic(self):
        failed = self._score("treatment", status="timeout", structured_answer=None)
        good = self._score("treatment")
        lonely = self._score("baseline", seed=2, run_key="claim-one::baseline::seed-2")
        agg = aggregate_claim_scores([failed, good, lonely])
        self.assertEqual(agg["arms"]["treatment"]["answered"], 1)
        self.assertEqual(agg["pairing"]["unpaired_count"], 2)

    def test_rejects_every_mixed_compatibility_dimension(self):
        base = [self._score("treatment"), self._score("baseline")]
        fields = {"task_bank_digest": "other", "forge_manifest": "other", "model": "other",
                  "mode": "other", "schema_version": "other", "settings": {"temperature": 1}}
        for field, value in fields.items():
            records = [dict(base[0]), dict(base[1])]
            records[1][field] = value
            with self.subTest(field=field), self.assertRaises(IncompatibleRuns):
                aggregate_claim_scores(records)


if __name__ == "__main__":
    unittest.main()
