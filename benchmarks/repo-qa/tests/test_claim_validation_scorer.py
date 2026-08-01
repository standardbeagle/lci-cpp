"""Hermetic contract tests for deterministic claim-validation scoring."""

import json
import os
import sys
import unittest
from tempfile import TemporaryDirectory
from unittest import mock

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "exploration")
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(os.path.dirname(ROOT), "scripts"))

from runner import run  # noqa: E402
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


class RunnerScorerParserParityTest(unittest.TestCase):
    """The runner gates an answer and the scorer scores it; if the two parsers
    disagree, a run is either rejected in flight but scoreable after the fact or
    accepted in flight and silently unscoreable. One parser, one verdict."""

    ACCEPTED = (
        {"verdict": "true", "evidence": [{"path": "src/live.py", "line": 3}],
         "rationale": "bounded"},
        {"verdict": "unsupported",
         "evidence": [{"path": "./packages/next/src/server/load-manifest.external.ts",
                       "line": 12}],
         "rationale": "the manifest reader is the anchor"},
    )
    REJECTED = (
        "not json at all",
        {"verdict": "yes", "evidence": [{"path": "a.py", "line": 1}], "rationale": "x"},
        {"verdict": "true", "evidence": [], "rationale": "x"},
        {"verdict": "true", "evidence": [{"path": "a.py", "line": 1}], "rationale": " "},
        {"verdict": "true", "evidence": [{"path": "../x.py", "line": 1}], "rationale": "x"},
        # Backslash-escaped traversal: rejected by the scorer, historically
        # accepted by the runner because os.path.isabs/split are POSIX-only.
        {"verdict": "true", "evidence": [{"path": "..\\secret/x.json", "line": 1}],
         "rationale": "x"},
        # Sealed citation: rejected by the runner, historically scored by the scorer.
        {"verdict": "true", "evidence": [{"path": "annotations/key.json", "line": 1}],
         "rationale": "x"},
        {"verdict": "true", "evidence": [{"path": "/etc/passwd", "line": 1}],
         "rationale": "x"},
        {"verdict": "true", "evidence": [{"path": "a.py", "line": True}], "rationale": "x"},
        {"verdict": "true", "evidence": [{"path": "a.py", "line": 0}], "rationale": "x"},
    )

    def _both(self, payload):
        raw = payload if isinstance(payload, str) else json.dumps(payload)
        runner_answer, runner_error = run._parse_claim_answer(raw)
        return (runner_answer is not None and runner_error is None,
                parse_claim_answer(raw) is not None)

    def test_runner_and_scorer_accept_the_same_payloads(self):
        for payload in self.ACCEPTED:
            with self.subTest(payload=payload):
                self.assertEqual(self._both(payload), (True, True))

    def test_runner_and_scorer_reject_the_same_payloads(self):
        for payload in self.REJECTED:
            with self.subTest(payload=payload):
                self.assertEqual(self._both(payload), (False, False))


class ScoringTests(unittest.TestCase):
    def test_all_verdicts_are_scored_deterministically(self):
        for verdict in ("true", "false", "unsupported"):
            with self.subTest(verdict=verdict):
                scored = score_claim_run(task(verdict=verdict), record(answer(verdict)),
                                         task_bank_digest="bank")
                self.assertEqual(scored["predicted_verdict"], verdict)
                self.assertTrue(scored["success"])

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

    def test_correct_but_uncited_guess_scores_zero(self):
        scored = score_claim_run(task(), record(answer(evidence=[{"path": "guess.py", "line": 1}])),
                                 task_bank_digest="bank")
        self.assertTrue(scored["verdict_correct"])
        self.assertFalse(scored["grounded"])
        self.assertFalse(scored["success"])
        self.assertEqual(scored["evidence"]["precision"], 0.0)

    def test_partial_anchor_set_reports_recall_and_obeys_threshold(self):
        scored = score_claim_run(task(), record(answer()), task_bank_digest="bank",
                                 settings={"evidence_min_recall": .5})
        self.assertEqual(scored["evidence"]["answer_key_matched"], 1)
        self.assertEqual(scored["evidence"]["recall"], .5)
        self.assertTrue(scored["success"])

    def test_each_forbidden_anchor_class_alone_scores_zero(self):
        for classification in ("wrong-layer", "misleading", "dead"):
            with self.subTest(classification=classification):
                scored = score_claim_run(task(classes=[classification, "authoritative-live"]),
                                         record(answer()), task_bank_digest="bank")
                self.assertEqual(scored["evidence"]["cited_valid"], 0)
                self.assertFalse(scored["success"])

    def test_claim_task_digest_is_preserved_in_score(self):
        scored = score_claim_run(task(), record(answer(), claim_task_digest="sha256:claim"),
                                 task_bank_digest="bank")
        self.assertEqual(scored["claim_task_digest"], "sha256:claim")

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

    def test_overlap_with_every_disallowed_anchor_class_fails_closed(self):
        for classification in ("misleading", "dead", "wrong-layer", "disallowed"):
            overlap = task(classes=["authoritative-live", classification])
            overlap["evidence"][1] = {"path": "src/live.py", "lines": [3]}
            with self.subTest(classification=classification):
                scored = score_claim_run(overlap, record(answer()), task_bank_digest="bank")
                self.assertFalse(scored["grounded"])
                self.assertEqual(scored["evidence"]["cited_invalid"], 1)


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

    def test_answered_retry_survives_later_timeout_and_failures_remain_forensic(self):
        failed = self._score("treatment", status="timeout", structured_answer=None)
        good = self._score("treatment")
        lonely = self._score("baseline", seed=2, run_key="claim-one::baseline::seed-2")
        agg = aggregate_claim_scores([good, failed, lonely])
        self.assertEqual(agg["arms"]["treatment"]["answered"], 1)
        self.assertEqual(agg["pairing"]["unpaired_count"], 2)
        history = next(row for row in agg["attempts"] if row["run_key"] == good["run_key"])
        self.assertEqual([attempt["status"] for attempt in history["attempts"]],
                         ["answered", "timeout"])
        self.assertEqual(agg["arms"]["treatment"]["grounded_accuracy"], 1.0)

    def test_answered_retry_survives_later_provider_error(self):
        good = self._score("treatment")
        failed = self._score("treatment", status="provider_error", structured_answer=None)
        agg = aggregate_claim_scores([good, failed])
        history = agg["attempts"][0]["attempts"]
        self.assertEqual([attempt["status"] for attempt in history],
                         ["answered", "provider_error"])
        self.assertEqual(agg["arms"]["treatment"]["grounded_accuracy"], 1.0)

    def test_incomplete_counterpart_is_not_used_for_paired_delta(self):
        treatment = self._score("treatment")
        baseline = self._score("baseline", status="provider_error", structured_answer=None)
        agg = aggregate_claim_scores([treatment, baseline])
        self.assertEqual(agg["pairing"]["paired_count"], 0)
        self.assertEqual(agg["pairing"]["unpaired_count"], 2)
        self.assertEqual(agg["deltas"], {})

    def test_latest_completed_retry_wins(self):
        wrong = self._score("treatment", verdict="true")
        right = self._score("treatment")
        agg = aggregate_claim_scores([wrong, right])
        self.assertEqual(agg["arms"]["treatment"]["verdict_accuracy"], 1.0)

    def test_rejects_every_mixed_compatibility_dimension(self):
        base = [self._score("treatment"), self._score("baseline")]
        fields = {"task_bank_digest": "other", "forge_manifest": "other", "model": "other",
                  "mode": "other", "schema_version": "other",
                  "run_settings": {"temperature": 1},
                  "scoring_settings": {"evidence_min_valid": 2}}
        for field, value in fields.items():
            records = [dict(base[0]), dict(base[1])]
            records[1][field] = value
            with self.subTest(field=field), self.assertRaises(IncompatibleRuns):
                aggregate_claim_scores(records)

    def test_explicit_scoring_policy_does_not_hide_incompatible_run_settings(self):
        policy = {"evidence_min_valid": 1}
        treatment = score_claim_run(
            task(), record(answer(), settings={"temperature": 0}),
            task_bank_digest="bank", settings=policy)
        baseline = score_claim_run(
            task(), record(answer(), arm="baseline", settings={"temperature": 1}),
            task_bank_digest="bank", settings=policy)
        self.assertEqual(treatment["run_settings"], {"temperature": 0})
        self.assertEqual(treatment["scoring_settings"], policy)
        with self.assertRaisesRegex(IncompatibleRuns, "run_settings"):
            aggregate_claim_scores([treatment, baseline])


class CliTests(unittest.TestCase):
    def test_outputs_retain_good_then_failed_retry_while_metrics_use_good_answer(self):
        import score_claim_validation

        for failure in ("timeout", "provider_error"):
            good = record(answer(), claim_task_digest="digest",
                          sealed_metadata={"task_id": "claim-one"})
            failed = record(None, status=failure, claim_task_digest="digest",
                            sealed_metadata={"task_id": "claim-one"})
            writes = {}

            def capture(path, value):
                writes[os.path.basename(path)] = value

            with self.subTest(failure=failure), TemporaryDirectory() as root, \
                    mock.patch.object(score_claim_validation, "_load_tasks",
                                      return_value={"claim-one": task()}), \
                    mock.patch.object(score_claim_validation.record_log, "load_records",
                                      return_value=[good, failed]), \
                    mock.patch.object(score_claim_validation, "_claim_digest", return_value="digest"), \
                    mock.patch.object(score_claim_validation, "_write", side_effect=capture):
                self.assertEqual(score_claim_validation.main([
                    "--tasks-dir", root, "--records", "records", "--out-dir", root,
                ]), 0)
            score = writes["scores.json"]["scores"][0]
            self.assertEqual(score["status"], "answered")
            self.assertEqual([attempt["status"] for attempt in score["attempts"]],
                             ["answered", failure])
            aggregate = writes["aggregate.json"]
            self.assertEqual(aggregate["arms"]["treatment"]["grounded_accuracy"], 1.0)
            self.assertEqual(aggregate["attempts"][0]["attempts"], score["attempts"])

    def test_absent_settings_override_preserves_records_and_rejects_mixture(self):
        import score_claim_validation

        first = score_claim_run(task(), record(answer(), settings={"temperature": 0}),
                                task_bank_digest="bank")
        second = score_claim_run(task(), record(answer(), arm="baseline",
                                                settings={"temperature": 1}),
                                 task_bank_digest="bank")
        with TemporaryDirectory() as root, \
                mock.patch.object(score_claim_validation, "_load_tasks", return_value={}), \
                mock.patch.object(score_claim_validation.record_log, "load_records", return_value=[]), \
                mock.patch.object(score_claim_validation, "score_bank", return_value=[first, second]) as scorer:
            with self.assertRaisesRegex(SystemExit, "incompatible 'run_settings'"):
                score_claim_validation.main(["--tasks-dir", root, "--records", "records",
                                             "--out-dir", os.path.join(root, "out")])
            self.assertIsNone(scorer.call_args.args[2])

    def test_explicit_settings_override_is_distinct(self):
        import score_claim_validation

        scored = score_claim_run(task(), record(answer()), task_bank_digest="bank",
                                 settings={"evidence_min_valid": 1})
        with TemporaryDirectory() as root, \
                mock.patch.object(score_claim_validation, "_load_tasks", return_value={}), \
                mock.patch.object(score_claim_validation.record_log, "load_records", return_value=[]), \
                mock.patch.object(score_claim_validation, "score_bank", return_value=[scored]) as scorer:
            self.assertEqual(score_claim_validation.main([
                "--tasks-dir", root, "--records", "records", "--out-dir", os.path.join(root, "out"),
                "--settings-json", '{"evidence_min_valid": 1}',
            ]), 0)
            self.assertEqual(scorer.call_args.args[2], {"evidence_min_valid": 1})

    def test_cli_explicit_policy_rejects_records_with_different_run_settings(self):
        import score_claim_validation

        policy = {"evidence_min_valid": 1}
        first = score_claim_run(task(), record(answer(), settings={"temperature": 0}),
                                task_bank_digest="bank", settings=policy)
        second = score_claim_run(task(), record(answer(), arm="baseline",
                                                settings={"temperature": 1}),
                                 task_bank_digest="bank", settings=policy)
        with TemporaryDirectory() as root, \
                mock.patch.object(score_claim_validation, "_load_tasks", return_value={}), \
                mock.patch.object(score_claim_validation.record_log, "load_records", return_value=[]), \
                mock.patch.object(score_claim_validation, "score_bank",
                                  return_value=[first, second]):
            with self.assertRaisesRegex(SystemExit, "incompatible 'run_settings'"):
                score_claim_validation.main([
                    "--tasks-dir", root, "--records", "records",
                    "--out-dir", os.path.join(root, "out"),
                    "--settings-json", '{"evidence_min_valid": 1}',
                ])


if __name__ == "__main__":
    unittest.main()
