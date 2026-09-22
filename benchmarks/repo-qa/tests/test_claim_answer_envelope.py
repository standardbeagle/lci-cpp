"""Envelope extraction for claim answers.

The answer contract is one JSON object with verdict, evidence and rationale.
Models deliver that object in different envelopes: bare, inside a Markdown
fence, or inside prose. Scoring only the bare form would record a formatting
habit as a wrong answer, and in the stage-2 pilot the treatment arm, which
explored longer, was the arm that wrapped its answer.

The fixture under tests/fixtures/claim_answers/ is verbatim: the final_answer
of the treatment cell skl-estimator-copy, model
cline-pass/cline-pass/deepseek-v4.1-flash, opencode 1.18.32, recorded
2026-09-22 by `exploration_runner.py --provider opencode claim-run`. It was
scored malformed_output (invalid_json) by the strict parser.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
EXPLORATION_ROOT = os.path.join(os.path.dirname(HERE), "exploration")
if EXPLORATION_ROOT not in sys.path:
    sys.path.insert(0, EXPLORATION_ROOT)

from scoring.claim_validation import (  # noqa: E402
    ENVELOPE_BARE, ENVELOPE_FENCED, ENVELOPE_PROSE_WRAPPED,
    extract_claim_answer, parse_claim_answer_result,
)

FIXTURE = os.path.join(HERE, "fixtures", "claim_answers",
                       "skl-estimator-copy.treatment.fenced.txt")
OBJ = '{"verdict":"false","evidence":[{"path":"a.py","line":3}],"rationale":"r"}'


class RecordedAnswerTest(unittest.TestCase):
    def test_the_recorded_fenced_answer_is_accepted_and_classed_fenced(self):
        with open(FIXTURE) as handle:
            text = handle.read()
        _value, envelope, reason = extract_claim_answer(text)
        self.assertIsNone(reason)
        self.assertEqual(envelope, ENVELOPE_FENCED)
        _answer, canonical, reason = parse_claim_answer_result(text)
        self.assertIsNone(reason)
        self.assertEqual(canonical["verdict"], "true")
        self.assertEqual(len(canonical["evidence"]), 4)


class EnvelopeClassTest(unittest.TestCase):
    """One case per envelope class, and one per rejection."""

    def assertEnvelope(self, text, envelope):
        _value, got, reason = extract_claim_answer(text)
        self.assertIsNone(reason)
        self.assertEqual(got, envelope)

    def assertRejected(self, text, reason):
        _value, envelope, got = extract_claim_answer(text)
        self.assertEqual(got, reason)
        self.assertIsNone(envelope)
        self.assertEqual(parse_claim_answer_result(text)[2], reason)

    def test_bare(self):
        self.assertEnvelope(OBJ, ENVELOPE_BARE)
        self.assertEnvelope("\n  " + OBJ + "\n", ENVELOPE_BARE)

    def test_fenced_with_a_language_tag(self):
        self.assertEnvelope("Here:\n```json\n" + OBJ + "\n```", ENVELOPE_FENCED)

    def test_fenced_without_a_language_tag(self):
        self.assertEnvelope("```\n" + OBJ + "\n```\nthanks", ENVELOPE_FENCED)

    def test_prose_wrapped_without_a_fence(self):
        self.assertEnvelope("Answer: " + OBJ + " -- done.", ENVELOPE_PROSE_WRAPPED)

    def test_two_answer_objects_are_ambiguous(self):
        self.assertRejected(OBJ + "\n" + OBJ, "ambiguous_answer")

    def test_a_truncated_object_is_not_accepted_as_a_fragment(self):
        self.assertRejected("```json\n" + OBJ[:-1], "invalid_json")

    def test_no_json_is_invalid(self):
        self.assertRejected("I could not determine this.", "invalid_json")

    def test_an_object_without_the_answer_fields_is_not_a_candidate(self):
        self.assertRejected('see {"verdict":"true"} above', "invalid_json")

    def test_an_answer_whose_fields_fail_validation_is_still_rejected(self):
        bad = '{"verdict":"maybe","evidence":[{"path":"a.py","line":3}],"rationale":"r"}'
        self.assertEqual(parse_claim_answer_result("```json\n" + bad + "\n```")[2], "verdict")

    def test_a_bare_non_answer_value_keeps_the_field_check(self):
        self.assertEqual(parse_claim_answer_result('{"verdict":"true"}')[2], "fields")


if __name__ == "__main__":
    unittest.main()
