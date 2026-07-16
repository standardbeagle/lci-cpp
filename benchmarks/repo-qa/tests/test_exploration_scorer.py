"""Contract tests for the Stage-1 exploration SCORER.

Every test is hermetic and network-free: a tiny adjudicated answer key is built
inline and S4-shaped run records are synthesised in-process. No real corpora, no
forged tree, no credentials, no LLM judge.

The bullets these tests pin (one acceptance criterion each, at least):
  * file:line citations parse out of prose into normalised mutated-corpus anchors;
  * exact single-line anchors match; a citation inside a bounded range matches;
    a line outside the declared bounds is a FALSE POSITIVE;
  * duplicate citations do NOT inflate precision or recall;
  * an out-of-corpus / unknown path is a FALSE POSITIVE;
  * an answer with prose but zero valid citations has zero evidence P/R/F1;
  * a failed run (timeout / provider_error / tool_violation / config_error) is
    preserved as an explicit status, NOT scored as an empty successful answer;
  * a paired aggregate reports per-arm counts, success rates, macro evidence,
    process distributions, and LCI-minus-baseline deltas;
  * an aggregate REFUSES mixed models / forge versions / task-bank versions
    unless the caller explicitly groups by that field.
"""

import json
import os
import sys
import unittest
from tempfile import TemporaryDirectory

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH_ROOT = os.path.dirname(HERE)
EXPLORATION_ROOT = os.path.join(BENCH_ROOT, "exploration")
SCRIPTS = os.path.join(BENCH_ROOT, "scripts")
for _p in (EXPLORATION_ROOT, SCRIPTS):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from scoring import (  # noqa: E402
    AGGREGATE_SCHEMA,
    SCORE_SCHEMA,
    IncompatibleRuns,
    aggregate,
    parse_citations,
    score_run,
)
from task_digest import task_digest  # noqa: E402

CORPUS_FILE = "packages/next/src/lib/redirect-status.ts"


def answer_key():
    """A minimal adjudicated task: one exact-line anchor and one bounded range."""
    return {
        "schema": "exploration_task_v1",
        "id": "demo-redirect",
        "corpus": "next.js",
        "manifest_ref": {
            "corpus_id": "next.js",
            "source_commit": "a" * 40,
            "seed": 7,
            "forge_version": "1",
        },
        "prompt": "Where is the permitted-status set declared and where is it chosen?",
        "rubric": {"must_surface": ["the set"], "answer_shape": "one set, one fn"},
        "evidence": [
            {
                "path": CORPUS_FILE,
                "lines": [3],
                "target_identifiers": ["allowedStatusCodes"],
                "claim": "declares the permitted numbers",
            },
            {
                "path": CORPUS_FILE,
                "lines": [5, 9],
                "target_identifiers": ["getRedirectStatus"],
                "claim": "selects the status number",
            },
        ],
        "adjudication": {"annotators": ["ann-hyde", "ann-quill"], "resolved": True},
    }


def make_record(
    task,
    *,
    arm="treatment",
    status="answered",
    final_answer="",
    tool_calls=3,
    input_tokens=100,
    output_tokens=40,
    duration=1.5,
    model="opus",
    forge_version="1",
    seed=None,
):
    """An S4-shaped per-run record (see runner/run.py `_finish`)."""
    ref = task["manifest_ref"]
    token_usage = (
        None
        if input_tokens is None
        else {"input": input_tokens, "output": output_tokens}
    )
    seed = ref["seed"] if seed is None else seed
    return {
        "run_key": f"{task['id']}::{arm}::seed-{seed}",
        "task_id": task["id"],
        "corpus_id": ref["corpus_id"],
        "source_commit": ref["source_commit"],
        "forge_version": forge_version,
        "seed": seed,
        "arm": arm,
        "model": model,
        "manifest_id": "tree-hash-xyz",
        "task_digest": task_digest(task),
        "status": status,
        "final_answer": final_answer,
        "tool_calls": [{"name": "Read", "arguments": {}} for _ in range(tool_calls)],
        "token_usage": token_usage,
        "duration_seconds": duration,
        "error": None if status == "answered" else status,
        "violations": [],
    }


class CitationParsingTests(unittest.TestCase):
    def test_parses_plain_and_ranged_and_decorated_forms(self):
        text = (
            f"The set is at {CORPUS_FILE}:3 and the routine spans "
            f"`{CORPUS_FILE}:5-9`; also see {CORPUS_FILE}:L7 for context."
        )
        cites = parse_citations(text)
        self.assertIn((CORPUS_FILE, 3, 3), cites)
        self.assertIn((CORPUS_FILE, 5, 9), cites)
        self.assertIn((CORPUS_FILE, 7, 7), cites)

    def test_is_sorted_and_deduped_deterministically(self):
        text = f"{CORPUS_FILE}:3 {CORPUS_FILE}:3 ./{CORPUS_FILE}:3"
        cites = parse_citations(text)
        self.assertEqual(cites, [(CORPUS_FILE, 3, 3)])

    def test_prose_without_a_line_is_not_a_citation(self):
        self.assertEqual(parse_citations("the allow-set is declared somewhere"), [])


class EvidenceScoringTests(unittest.TestCase):
    def test_exact_and_bounded_anchors_both_match(self):
        task = answer_key()
        answer = f"Declared at {CORPUS_FILE}:3, selected at {CORPUS_FILE}:7."
        score = score_run(task, make_record(task, final_answer=answer))
        ev = score["evidence"]
        self.assertEqual(ev["precision"], 1.0)
        self.assertEqual(ev["recall"], 1.0)
        self.assertEqual(ev["f1"], 1.0)
        self.assertEqual(ev["answer_key_matched"], 2)

    def test_line_outside_declared_bounds_is_false_positive(self):
        task = answer_key()
        # line 4 is in neither [3] nor [5,9]
        answer = f"See {CORPUS_FILE}:4."
        ev = score_run(task, make_record(task, final_answer=answer))["evidence"]
        self.assertEqual(ev["cited_total"], 1)
        self.assertEqual(ev["cited_valid"], 0)
        self.assertEqual(ev["precision"], 0.0)
        self.assertEqual(ev["recall"], 0.0)

    def test_partial_recall_on_single_bounded_hit(self):
        task = answer_key()
        answer = f"Only the routine, at {CORPUS_FILE}:8."
        ev = score_run(task, make_record(task, final_answer=answer))["evidence"]
        self.assertEqual(ev["precision"], 1.0)
        self.assertEqual(ev["recall"], 0.5)

    def test_duplicate_citations_do_not_inflate(self):
        task = answer_key()
        answer = f"{CORPUS_FILE}:3, and again {CORPUS_FILE}:3, and {CORPUS_FILE}:3."
        ev = score_run(task, make_record(task, final_answer=answer))["evidence"]
        self.assertEqual(ev["cited_total"], 1)
        self.assertEqual(ev["cited_valid"], 1)
        self.assertEqual(ev["precision"], 1.0)
        self.assertEqual(ev["recall"], 0.5)

    def test_out_of_corpus_path_is_false_positive(self):
        task = answer_key()
        answer = f"Real: {CORPUS_FILE}:3. Bogus: totally/made-up/thing.ts:3."
        ev = score_run(task, make_record(task, final_answer=answer))["evidence"]
        self.assertEqual(ev["cited_total"], 2)
        self.assertEqual(ev["cited_valid"], 1)
        self.assertEqual(ev["cited_invalid"], 1)
        self.assertEqual(ev["precision"], 0.5)

    def test_whole_file_citation_does_not_match_a_bounded_anchor(self):
        task = answer_key()
        answer = f"The whole file {CORPUS_FILE}:1-400 is relevant."
        ev = score_run(task, make_record(task, final_answer=answer))["evidence"]
        self.assertEqual(ev["cited_valid"], 0)
        self.assertEqual(ev["precision"], 0.0)

    def test_uncited_prose_answer_has_zero_evidence(self):
        task = answer_key()
        answer = "The allow-set is declared and a routine selects the status."
        score = score_run(task, make_record(task, final_answer=answer))
        ev = score["evidence"]
        self.assertTrue(score["answered"])
        self.assertEqual(ev["cited_total"], 0)
        self.assertEqual(ev["precision"], 0.0)
        self.assertEqual(ev["recall"], 0.0)
        self.assertEqual(ev["f1"], 0.0)


class FailedRunTests(unittest.TestCase):
    def test_timeout_is_preserved_not_scored_as_empty_answer(self):
        task = answer_key()
        rec = make_record(task, status="timeout", final_answer=None)
        score = score_run(task, rec)
        self.assertEqual(score["status"], "timeout")
        self.assertFalse(score["answered"])
        ev = score["evidence"]
        self.assertIsNone(ev["precision"])
        self.assertIsNone(ev["recall"])
        self.assertIsNone(ev["f1"])

    def test_config_error_with_null_tokens_is_handled(self):
        task = answer_key()
        rec = make_record(
            task,
            status="config_error",
            final_answer=None,
            tool_calls=0,
            input_tokens=None,
        )
        score = score_run(task, rec)
        self.assertEqual(score["status"], "config_error")
        self.assertFalse(score["answered"])
        self.assertEqual(score["process"]["tool_calls"], 0)
        self.assertIsNone(score["process"]["input_tokens"])

    def test_score_record_is_schema_versioned(self):
        task = answer_key()
        score = score_run(task, make_record(task, final_answer=f"{CORPUS_FILE}:3"))
        self.assertEqual(score["schema"], SCORE_SCHEMA)
        self.assertEqual(score["task_bank_version"], "exploration_task_v1")


class AggregateTests(unittest.TestCase):
    def _paired_scores(self):
        task = answer_key()
        good = f"Declared {CORPUS_FILE}:3, selected {CORPUS_FILE}:7."
        weak = f"Maybe {CORPUS_FILE}:4."  # invalid line -> zero evidence
        return [
            score_run(task, make_record(task, arm="treatment", final_answer=good,
                                        tool_calls=4, duration=1.0, seed=7)),
            score_run(task, make_record(task, arm="treatment", final_answer=good,
                                        tool_calls=6, duration=2.0, seed=8)),
            score_run(task, make_record(task, arm="baseline", final_answer=weak,
                                        tool_calls=10, duration=5.0, seed=7)),
            score_run(task, make_record(task, arm="baseline", final_answer=weak,
                                        tool_calls=12, duration=7.0, seed=8)),
        ]

    def test_paired_aggregate_reports_arms_and_deltas(self):
        agg = aggregate(self._paired_scores())
        self.assertEqual(agg["schema"], AGGREGATE_SCHEMA)
        arms = agg["arms"]
        self.assertEqual(arms["treatment"]["count"], 2)
        self.assertEqual(arms["baseline"]["count"], 2)
        self.assertEqual(arms["treatment"]["success_rate"], 1.0)
        self.assertEqual(arms["treatment"]["evidence"]["precision"], 1.0)
        self.assertEqual(arms["baseline"]["evidence"]["precision"], 0.0)
        # process distribution present
        self.assertEqual(arms["treatment"]["tool_calls"]["mean"], 5.0)
        # LCI-minus-baseline deltas
        self.assertEqual(agg["deltas"]["precision"], 1.0)
        self.assertEqual(agg["deltas"]["recall"], 1.0)
        self.assertLess(agg["deltas"]["wall_clock_mean"], 0.0)
        self.assertEqual(agg["pairing"]["paired_count"], 2)
        self.assertEqual(agg["pairing"]["unpaired"], [])

    def test_latest_duplicate_run_key_wins(self):
        task = answer_key()
        failed = score_run(task, make_record(
            task, arm="treatment", status="timeout", final_answer=None
        ))
        succeeded = score_run(task, make_record(
            task, arm="treatment", final_answer=f"{CORPUS_FILE}:3"
        ))
        baseline = score_run(task, make_record(
            task, arm="baseline", final_answer=f"{CORPUS_FILE}:3"
        ))
        agg = aggregate([failed, succeeded, baseline])
        self.assertEqual(agg["arms"]["treatment"]["count"], 1)
        self.assertEqual(agg["arms"]["treatment"]["answered"], 1)
        self.assertEqual(agg["pairing"]["paired_count"], 1)

    def test_missing_or_mismatched_cells_are_unpaired_and_not_in_deltas(self):
        task = answer_key()
        good = f"{CORPUS_FILE}:3 and {CORPUS_FILE}:7"
        weak = f"{CORPUS_FILE}:4"
        scores = [
            score_run(task, make_record(task, arm="treatment", seed=7,
                                        final_answer=good)),
            score_run(task, make_record(task, arm="baseline", seed=7,
                                        final_answer=weak)),
            score_run(task, make_record(task, arm="treatment", seed=8,
                                        final_answer=weak)),
            score_run(task, make_record(task, arm="baseline", seed=9,
                                        final_answer=good)),
        ]
        agg = aggregate(scores)
        self.assertEqual(agg["pairing"]["paired_count"], 1)
        self.assertEqual(agg["pairing"]["unpaired_count"], 2)
        self.assertEqual(agg["deltas"]["precision"], 1.0)
        self.assertEqual(
            {(cell["arm"], cell["seed"]) for cell in agg["pairing"]["unpaired"]},
            {("treatment", 8), ("baseline", 9)},
        )

    def test_rejects_distinct_run_keys_claiming_same_cell(self):
        task = answer_key()
        first = score_run(task, make_record(task, arm="treatment"))
        second_record = make_record(task, arm="treatment")
        second_record["run_key"] = "legacy-distinct-key"
        second = score_run(task, second_record)
        baseline = score_run(task, make_record(task, arm="baseline"))
        with self.assertRaisesRegex(IncompatibleRuns, "same task/seed/arm cell"):
            aggregate([first, second, baseline])
        with self.assertRaisesRegex(IncompatibleRuns, "same task/seed/arm cell"):
            aggregate([second, first, baseline])

    def test_rejects_missing_run_key(self):
        task = answer_key()
        rec = make_record(task)
        rec["run_key"] = None
        with self.assertRaisesRegex(IncompatibleRuns, "missing required run_key"):
            aggregate([score_run(task, rec)])

    def test_refuses_mixed_models(self):
        task = answer_key()
        scores = [
            score_run(task, make_record(task, arm="treatment", model="opus",
                                        final_answer=f"{CORPUS_FILE}:3")),
            score_run(task, make_record(task, arm="baseline", model="sonnet",
                                        final_answer=f"{CORPUS_FILE}:3")),
        ]
        with self.assertRaises(IncompatibleRuns):
            aggregate(scores)
        # explicit grouping opts out of the refusal
        agg = aggregate(scores, group_by=["model"])
        self.assertIn("model", agg["grouped_by"])

    def test_refuses_mixed_forge_versions(self):
        task = answer_key()
        scores = [
            score_run(task, make_record(task, arm="treatment", forge_version="1",
                                        final_answer=f"{CORPUS_FILE}:3")),
            score_run(task, make_record(task, arm="baseline", forge_version="2",
                                        final_answer=f"{CORPUS_FILE}:3")),
        ]
        with self.assertRaises(IncompatibleRuns):
            aggregate(scores)


class CliTests(unittest.TestCase):
    def _write_bank(self, root):
        tasks_dir = os.path.join(root, "tasks")
        os.makedirs(tasks_dir)
        task = answer_key()
        with open(os.path.join(tasks_dir, task["id"] + ".json"), "w") as handle:
            json.dump(task, handle)
        records_path = os.path.join(root, "records.jsonl")
        recs = [
            make_record(task, arm="treatment",
                        final_answer=f"{CORPUS_FILE}:3 and {CORPUS_FILE}:7"),
            make_record(task, arm="baseline", status="timeout", final_answer=None),
        ]
        with open(records_path, "w") as handle:
            for rec in recs:
                handle.write(json.dumps(rec) + "\n")
        return tasks_dir, records_path

    def test_cli_emits_scores_and_aggregate_json(self):
        import score_exploration

        with TemporaryDirectory() as root:
            tasks_dir, records_path = self._write_bank(root)
            out_dir = os.path.join(root, "out")
            rc = score_exploration.main(
                ["--tasks-dir", tasks_dir, "--records", records_path,
                 "--out-dir", out_dir]
            )
            self.assertEqual(rc, 0)
            with open(os.path.join(out_dir, "scores.json")) as handle:
                scores = json.load(handle)
            self.assertEqual(scores["schema"], "exploration_score_set_v1")
            self.assertEqual(len(scores["scores"]), 2)
            with open(os.path.join(out_dir, "aggregate.json")) as handle:
                agg = json.load(handle)
            self.assertEqual(agg["schema"], AGGREGATE_SCHEMA)
            # timeout preserved: baseline answered 0/1, treatment answered 1/1
            self.assertEqual(agg["arms"]["baseline"]["answered"], 0)
            self.assertEqual(agg["arms"]["treatment"]["evidence"]["precision"], 1.0)

    def test_cli_fails_loud_on_unknown_task_id(self):
        import score_exploration

        with TemporaryDirectory() as root:
            tasks_dir, records_path = self._write_bank(root)
            # append a record referencing a task not in the bank
            with open(records_path, "a") as handle:
                handle.write(json.dumps({"task_id": "ghost", "arm": "treatment",
                                         "status": "answered"}) + "\n")
            with self.assertRaises(SystemExit) as caught:
                score_exploration.main(
                    ["--tasks-dir", tasks_dir, "--records", records_path,
                     "--out-dir", os.path.join(root, "out")]
                )
            self.assertNotEqual(caught.exception.code, 0)

    def test_cli_rejects_run_when_answer_key_digest_changed(self):
        import score_exploration

        with TemporaryDirectory() as root:
            tasks_dir, records_path = self._write_bank(root)
            task_path = os.path.join(tasks_dir, answer_key()["id"] + ".json")
            with open(task_path) as handle:
                changed = json.load(handle)
            changed["evidence"][0]["lines"] = [30]
            with open(task_path, "w") as handle:
                json.dump(changed, handle)
            with self.assertRaisesRegex(SystemExit, "task digest does not match"):
                score_exploration.main([
                    "--tasks-dir", tasks_dir, "--records", records_path,
                    "--out-dir", os.path.join(root, "out"),
                ])

    def test_cli_rejects_legacy_run_without_task_digest(self):
        import score_exploration

        with TemporaryDirectory() as root:
            tasks_dir, records_path = self._write_bank(root)
            records = []
            with open(records_path) as handle:
                for line in handle:
                    record = json.loads(line)
                    record.pop("task_digest")
                    records.append(record)
            with open(records_path, "w") as handle:
                for record in records:
                    handle.write(json.dumps(record) + "\n")
            with self.assertRaisesRegex(SystemExit, "task digest missing"):
                score_exploration.main([
                    "--tasks-dir", tasks_dir, "--records", records_path,
                    "--out-dir", os.path.join(root, "out"),
                ])

    def test_cli_scores_latest_retry_and_collapses_duplicate_completed_records(self):
        import score_exploration

        with TemporaryDirectory() as root:
            tasks_dir, records_path = self._write_bank(root)
            task = answer_key()
            with open(records_path, "a") as handle:
                handle.write(json.dumps(make_record(
                    task, arm="baseline", final_answer=f"{CORPUS_FILE}:4"
                )) + "\n")
                handle.write(json.dumps(make_record(
                    task, arm="baseline",
                    final_answer=f"{CORPUS_FILE}:3 and {CORPUS_FILE}:7"
                )) + "\n")
            out_dir = os.path.join(root, "out")
            self.assertEqual(score_exploration.main([
                "--tasks-dir", tasks_dir, "--records", records_path,
                "--out-dir", out_dir,
            ]), 0)
            with open(os.path.join(out_dir, "scores.json")) as handle:
                scores = json.load(handle)["scores"]
            self.assertEqual(len(scores), 2)
            baseline = next(s for s in scores if s["arm"] == "baseline")
            self.assertEqual(baseline["status"], "answered")
            self.assertEqual(baseline["evidence"]["recall"], 1.0)

    def test_cli_rejects_unkeyed_record(self):
        import score_exploration

        with TemporaryDirectory() as root:
            tasks_dir, records_path = self._write_bank(root)
            with open(records_path, "a") as handle:
                handle.write(json.dumps({"task_id": answer_key()["id"]}) + "\n")
            with self.assertRaisesRegex(SystemExit, "missing required run_key"):
                score_exploration.main([
                    "--tasks-dir", tasks_dir, "--records", records_path,
                    "--out-dir", os.path.join(root, "out"),
                ])

    def test_cli_empty_aggregate_has_stable_schema_shape(self):
        import score_exploration

        with TemporaryDirectory() as root:
            tasks_dir = os.path.join(root, "tasks")
            os.makedirs(tasks_dir)
            records_path = os.path.join(root, "records.jsonl")
            with open(records_path, "w"):
                pass
            out_dir = os.path.join(root, "out")
            self.assertEqual(score_exploration.main([
                "--tasks-dir", tasks_dir, "--records", records_path,
                "--out-dir", out_dir, "--group-by", "model",
            ]), 0)
            with open(os.path.join(out_dir, "aggregate.json")) as handle:
                agg = json.load(handle)
            self.assertEqual(agg["grouped_by"], ["model"])
            self.assertEqual(agg["compatibility"], {})
            self.assertEqual(agg["pairing"], {
                "paired_count": 0, "unpaired_count": 0, "unpaired": [],
            })


if __name__ == "__main__":
    unittest.main()
