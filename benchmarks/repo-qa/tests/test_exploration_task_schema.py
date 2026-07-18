#!/usr/bin/env python3
"""Unit tests for the stage-1 exploration task validator.

Hermetic: every live-anchor test builds a synthetic forged corpus (a tiny tree +
a manifest in the exact shape the forge emits) in a tempdir. Nothing here needs
the network or the multi-gigabyte real corpora, so the suite runs anywhere via

    python3 -m unittest discover -s benchmarks/repo-qa/tests

The final section additionally validates the REAL committed task bank against the
schema and the structural rules (dual annotation, leakage) WITHOUT live anchors,
so a malformed committed task is caught even where the forged tree is absent.
"""

import copy
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"
)
sys.path.insert(0, SCRIPTS)

import validate_exploration_tasks as vet  # noqa: E402

CORPUS_ID = "pocketbase"
SEED = 99
FILE_REL = "core/base.go"
FILE_BODY = "\n".join(
    [
        "package core",                            # 1
        "",                                        # 2
        "// App is the central application.",      # 3
        "type App struct {",                       # 4
        "\tstore *Store",                          # 5
        "}",                                        # 6
        "",                                        # 7
        "func NewBaseApp(cfg Config) *App {",      # 8
        "\treturn &App{store: newStore(cfg)}",     # 9
        "}",                                        # 10
        "",                                        # 11
        "func (a *App) Bootstrap() error {",       # 12
        "\treturn a.store.open()",                 # 13
        "}",                                        # 14
    ]
) + "\n"


def _pinned_commit():
    corpora = vet.load_corpora()
    return corpora[CORPUS_ID]["pinned_commit"]


def _write(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        if isinstance(obj, str):
            handle.write(obj)
        else:
            json.dump(obj, handle, indent=2)


class Fixture:
    """A self-contained, schema-valid task bank on disk plus its forged corpus."""

    def __init__(self, root):
        self.root = root
        self.tasks_dir = os.path.join(root, "tasks")
        self.annotations_dir = os.path.join(root, "annotations")
        self.corpus_root = os.path.join(root, "corpus_root")
        self.commit = _pinned_commit()

        tree = os.path.join(self.corpus_root, CORPUS_ID, f"seed-{SEED}", "tree")
        _write(os.path.join(tree, FILE_REL), FILE_BODY)
        manifest = {
            "schema": "exploration_corpus_manifest_v1",
            "corpus_id": CORPUS_ID,
            "source_commit": self.commit,
            "seed": SEED,
            "forge_version": vet.forge.FORGE_VERSION,
            "path_map": {FILE_REL: FILE_REL},
            "decoys": [],
            "tree_hash": "0" * 64,
            "status": "ready",
        }
        _write(
            os.path.join(self.corpus_root, CORPUS_ID, f"seed-{SEED}", "manifest.json"),
            manifest,
        )

        self.task = {
            "schema": "exploration_task_v1",
            "id": "pb-app-bootstrap",
            "corpus": CORPUS_ID,
            "manifest_ref": {
                "corpus_id": CORPUS_ID,
                "source_commit": self.commit,
                "seed": SEED,
                "forge_version": vet.forge.FORGE_VERSION,
            },
            "prompt": (
                "When this backend first comes up, where does it wire the "
                "central object together, and how does it start its persistence?"
            ),
            "claim": "The startup behavior described in the request holds.",
            "request": "Determine whether the startup and persistence behavior is implemented.",
            "rubric": {
                "must_surface": ["the constructor", "the bootstrap path"],
                "answer_shape": "one location for construction, one for startup",
            },
            "evidence": [
                {
                    "path": FILE_REL,
                    "lines": [8, 10],
                    "claim": "the base app constructor",
                    "target_identifiers": ["NewBaseApp"],
                },
                {
                    "path": FILE_REL,
                    "lines": [12, 14],
                    "claim": "startup opens the store",
                    "target_identifiers": ["Bootstrap"],
                },
            ],
            "adjudication": {
                "annotators": ["ann-a", "ann-b"],
                "resolved": True,
                "notes": "concurring",
            },
            "author": {
                "verdict": "true",
                "category": "true",
                "adjudication_notes": "Two independent annotations concurred.",
                "anchor_classification": ["authoritative-live", "authoritative-live"],
            },
        }
        self._annotation("ann-a")
        self._annotation("ann-b")
        self._flush_task()

    def _annotation(self, annotator, evidence=None, classifications=None):
        record = {
            "schema": "exploration_annotation_v1",
            "task_id": self.task["id"],
            "annotator": annotator,
            "verdict": self.task["author"]["verdict"],
            "evidence": copy.deepcopy(
                evidence if evidence is not None else self.task["evidence"]
            ),
            "anchor_classification": copy.deepcopy(
                classifications
                if classifications is not None
                else self.task["author"]["anchor_classification"]
            ),
        }
        _write(
            os.path.join(self.annotations_dir, f"{self.task['id']}.{annotator}.json"),
            record,
        )

    def _flush_task(self):
        _write(
            os.path.join(self.tasks_dir, f"{self.task['id']}.json"), self.task
        )

    def run(self, require_live=True):
        return vet.validate_task(
            self.task,
            vet.load_schema(),
            vet.load_corpora(),
            self.annotations_dir,
            self.corpus_root,
            require_live,
        )


class ValidatorTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_valid_fixture_passes(self):
        self.assertEqual(self.fx.run(), [])

    def test_missing_second_annotation_fails(self):
        os.remove(
            os.path.join(self.fx.annotations_dir, "pb-app-bootstrap.ann-b.json")
        )
        self.assertTrue(self.fx.run())

    def test_unresolved_disagreement_fails(self):
        # ann-b disagrees (different line bound) and the task is not adjudicated.
        alt = copy.deepcopy(self.fx.task["evidence"])
        alt[0]["lines"] = [8, 9]
        self.fx._annotation("ann-b", alt)
        self.fx.task["adjudication"]["resolved"] = False
        self.fx._flush_task()
        self.assertTrue(any("disagree" in p for p in self.fx.run()))

    def test_annotation_without_independent_verdict_fails(self):
        path = os.path.join(
            self.fx.annotations_dir, "pb-app-bootstrap.ann-b.json"
        )
        record = vet._load_json(path)
        del record["verdict"]
        _write(path, record)
        self.assertTrue(any("independent verdict" in p for p in self.fx.run()))

    def test_annotation_without_independent_classification_fails(self):
        path = os.path.join(
            self.fx.annotations_dir, "pb-app-bootstrap.ann-b.json"
        )
        record = vet._load_json(path)
        del record["anchor_classification"]
        _write(path, record)
        self.assertTrue(
            any("independent anchor classification" in p for p in self.fx.run())
        )

    def test_live_anchor_misclassification_fails(self):
        wrong = ["dead", "authoritative-live"]
        self.fx.task["author"]["anchor_classification"] = wrong
        self.fx._annotation("ann-a", classifications=wrong)
        self.fx._annotation("ann-b", classifications=wrong)
        self.fx._flush_task()
        self.assertTrue(
            any("contradicts pinned manifest/live evidence" in p for p in self.fx.run())
        )

    def test_annotation_misclassification_fails_even_when_adjudication_is_live(self):
        wrong = ["dead", "authoritative-live"]
        self.fx._annotation("ann-a", classifications=wrong)
        self.assertTrue(
            any("ann-a" in p and "contradicts pinned" in p for p in self.fx.run())
        )

    def test_dead_decoy_misclassified_as_authoritative_fails(self):
        manifest_path = os.path.join(
            self.fx.corpus_root, CORPUS_ID, f"seed-{SEED}", "manifest.json"
        )
        manifest = vet._load_json(manifest_path)
        manifest["decoys"] = [{"path": FILE_REL}]
        _write(manifest_path, manifest)
        self.assertTrue(
            any("('dead')" in p for p in self.fx.run())
        )

    def test_misleading_forge_trap_misclassified_as_authoritative_fails(self):
        tree_path = os.path.join(
            self.fx.corpus_root, CORPUS_ID, f"seed-{SEED}", "tree", FILE_REL
        )
        lines = FILE_BODY.splitlines()
        lines[2] = "// FORGE TRAP: App is obsolete and never used."
        _write(tree_path, "\n".join(lines) + "\n")
        self.fx.task["evidence"][0] = {
            "path": FILE_REL,
            "lines": [3],
            "claim": "a misleading injected statement",
            "target_identifiers": ["App"],
        }
        self.fx._annotation("ann-a")
        self.fx._annotation("ann-b")
        self.fx._flush_task()
        self.assertTrue(
            any("('misleading')" in p for p in self.fx.run())
        )

    def test_fewer_than_two_anchors_fails(self):
        self.fx.task["evidence"] = self.fx.task["evidence"][:1]
        self.fx._annotation("ann-a", self.fx.task["evidence"])
        self.fx._annotation("ann-b", self.fx.task["evidence"])
        self.fx._flush_task()
        self.assertTrue(self.fx.run())

    def test_stale_line_bound_fails(self):
        self.fx.task["evidence"][0]["lines"] = [8, 999]
        self.fx._annotation("ann-a", self.fx.task["evidence"])
        self.fx._annotation("ann-b", self.fx.task["evidence"])
        self.fx._flush_task()
        self.assertTrue(any("exceeds file length" in p for p in self.fx.run()))

    def test_identifier_not_in_bounds_fails(self):
        # NewBaseApp is on lines 8-10; bound only line 12-14 where it is absent.
        self.fx.task["evidence"][0]["lines"] = [12, 14]
        self.fx._annotation("ann-a", self.fx.task["evidence"])
        self.fx._annotation("ann-b", self.fx.task["evidence"])
        self.fx._flush_task()
        self.assertTrue(
            any("does not occur" in p for p in self.fx.run())
        )

    def test_prompt_leakage_of_identifier_fails(self):
        self.fx.task["prompt"] = (
            "Where is NewBaseApp defined and how does Bootstrap start the store?"
        )
        self.fx._flush_task()
        self.assertTrue(any("leaks target identifier" in p for p in self.fx.run()))

    def test_absent_corpus_with_require_live_fails(self):
        self.fx.task["manifest_ref"]["seed"] = 12345  # no such forged tree
        self.fx._flush_task()
        self.assertTrue(any("not found" in p for p in self.fx.run(require_live=True)))

    def test_absent_corpus_without_require_live_skips_live(self):
        self.fx.task["manifest_ref"]["seed"] = 12345
        self.fx._flush_task()
        # Structural layer still passes; live checks are skipped, not failed.
        self.assertEqual(self.fx.run(require_live=False), [])

    def test_schema_violation_fails(self):
        del self.fx.task["rubric"]
        self.fx._flush_task()
        self.assertTrue(any("schema violation" in p for p in self.fx.run()))

    def test_stale_mutated_path_fails(self):
        self.fx.task["evidence"][0]["path"] = "core/does-not-exist.go"
        self.fx._annotation("ann-a", self.fx.task["evidence"])
        self.fx._annotation("ann-b", self.fx.task["evidence"])
        self.fx._flush_task()
        self.assertTrue(self.fx.run())

    def test_success_output_publishes_complete_category_and_verdict_counts(self):
        summary = {
            "tasks": 30,
            "per_corpus": {"pocketbase": 10, "next.js": 10, "scikit-learn": 10},
            "per_category": {
                "true": 5,
                "wrong-layer": 5,
                "misleading-doc": 5,
                "dead-code": 5,
                "false-premise": 5,
                "unsupported": 5,
            },
            "per_verdict": {"true": 10, "false": 10, "unsupported": 10},
        }
        stdout = io.StringIO()

        with mock.patch.object(vet, "validate_bank", return_value=([], summary)):
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(vet.main([]), 0)

        output = stdout.getvalue()
        self.assertIn(f"categories={summary['per_category']}", output)
        self.assertIn(f"verdicts={summary['per_verdict']}", output)
        for verdict in ("true", "false", "unsupported"):
            self.assertIn(f"'{verdict}':", output)


class RealTaskBankTest(unittest.TestCase):
    """The committed bank must be schema+structurally valid without a live tree."""

    def test_bank_size_and_corpora_coverage(self):
        problems, summary = vet.validate_bank(require_live=False)
        self.assertEqual(problems, [], msg="\n".join(problems))
        self.assertGreaterEqual(summary["tasks"], 30)
        self.assertLessEqual(summary["tasks"], 50)
        self.assertEqual(
            set(summary["per_corpus"]),
            {"scikit-learn", "pocketbase", "next.js"},
        )
        for corpus, count in summary["per_corpus"].items():
            self.assertGreater(count, 0, msg=f"{corpus} has no tasks")
        self.assertEqual(set(summary["per_category"]), {
            "true", "wrong-layer", "misleading-doc", "dead-code",
            "false-premise", "unsupported",
        })
        self.assertEqual(set(summary["per_verdict"]), vet.EXPECTED_VERDICTS)
        self.assertLessEqual(max(summary["per_category"].values()), summary["tasks"] / 2)
        self.assertLessEqual(max(summary["per_verdict"].values()), summary["tasks"] / 2)

    def test_bank_size_boundaries_are_enforced_hermetically(self):
        for count, expected_problem in ((29, True), (30, False), (50, False), (51, True)):
            with self.subTest(count=count):
                problems = self._composition_problems(count)
                expected = (
                    [f"bank must contain 30-50 tasks; got {count}"]
                    if expected_problem else []
                )
                self.assertEqual(problems, expected)

    def test_bank_imbalance_is_rejected_below_thirty_tasks(self):
        problems = self._composition_problems(29, majority=True)
        self.assertEqual(problems, [
            "bank must contain 30-50 tasks; got 29",
            "bank must cover every verdict; got ['true']",
            "bank balance limit exceeded: no category or verdict may be a majority",
        ])

    def test_each_missing_corpus_is_rejected_hermetically(self):
        for missing in sorted(vet.EXPECTED_CORPORA):
            with self.subTest(missing=missing):
                problems = self._composition_problems(30, missing_corpus=missing)
                covered = sorted(vet.EXPECTED_CORPORA - {missing})
                self.assertEqual(
                    problems, [f"bank must cover every corpus; got {covered}"]
                )

    def test_each_missing_category_is_rejected_hermetically(self):
        for missing in sorted(vet.EXPECTED_CATEGORIES):
            with self.subTest(missing=missing):
                problems = self._composition_problems(30, missing_category=missing)
                covered = sorted(vet.EXPECTED_CATEGORIES - {missing})
                self.assertEqual(
                    problems, [f"bank must cover every category; got {covered}"]
                )

    def test_each_missing_verdict_is_rejected_hermetically(self):
        for missing in sorted(vet.EXPECTED_VERDICTS):
            with self.subTest(missing=missing):
                problems = self._composition_problems(30, missing_verdict=missing)
                covered = sorted(vet.EXPECTED_VERDICTS - {missing})
                self.assertEqual(
                    problems, [f"bank must cover every verdict; got {covered}"]
                )

    def test_category_majority_is_rejected_hermetically(self):
        problems = self._composition_problems(30, category_majority=True)
        self.assertEqual(problems, [
            "bank balance limit exceeded: no category or verdict may be a majority"
        ])

    def test_verdict_majority_is_rejected_hermetically(self):
        problems = self._composition_problems(30, verdict_majority=True)
        self.assertEqual(problems, [
            "bank balance limit exceeded: no category or verdict may be a majority"
        ])

    def _composition_problems(
        self,
        count,
        majority=False,
        missing_corpus=None,
        missing_category=None,
        missing_verdict=None,
        category_majority=False,
        verdict_majority=False,
    ):
        with tempfile.TemporaryDirectory() as root:
            tasks = os.path.join(root, "tasks")
            annotations = os.path.join(root, "annotations")
            os.makedirs(tasks)
            os.makedirs(annotations)
            corpora = sorted(vet.EXPECTED_CORPORA)
            categories = sorted(vet.EXPECTED_CATEGORIES)
            if missing_corpus is not None:
                corpora.remove(missing_corpus)
            if missing_category is not None:
                categories.remove(missing_category)
            verdicts = sorted(vet.EXPECTED_VERDICTS)
            if missing_verdict is not None:
                verdicts.remove(missing_verdict)
            template_root = os.path.join(root, "template")
            template = Fixture(template_root).task
            corpus_specs = vet.load_corpora()
            for index in range(count):
                task = copy.deepcopy(template)
                task["id"] = f"task-{index}"
                task["corpus"] = corpora[index % len(corpora)]
                task["manifest_ref"]["corpus_id"] = task["corpus"]
                task["manifest_ref"]["source_commit"] = corpus_specs[
                    task["corpus"]
                ]["pinned_commit"]
                category = (
                    categories[0]
                    if category_majority and index < count // 2 + 1
                    else categories[index % len(categories)]
                )
                verdict = (
                    "true"
                    if majority or (verdict_majority and index < count // 2 + 1)
                    else verdicts[index % len(verdicts)]
                )
                task["author"]["category"] = category
                task["author"]["verdict"] = verdict
                for field in (
                    "falsification_evidence", "search_boundary",
                    "abstraction_boundary",
                ):
                    task["author"].pop(field, None)
                if verdict in {"false", "unsupported"}:
                    task["author"]["falsification_evidence"] = (
                        "The bounded evidence does not establish the claim."
                    )
                    task["author"]["search_boundary"] = (
                        "The synthetic fixture evidence was searched."
                    )
                if category == "wrong-layer":
                    task["author"]["abstraction_boundary"] = (
                        "The behavior belongs to a different layer."
                    )
                _write(os.path.join(tasks, f"task-{index}.json"), task)
                for annotator in task["adjudication"]["annotators"]:
                    _write(
                        os.path.join(
                            annotations, f"task-{index}.{annotator}.json"
                        ),
                        {
                            "schema": "exploration_annotation_v1",
                            "task_id": task["id"],
                            "annotator": annotator,
                            "verdict": verdict,
                            "evidence": copy.deepcopy(task["evidence"]),
                            "anchor_classification": copy.deepcopy(
                                task["author"]["anchor_classification"]
                            ),
                        },
                    )
            return vet.validate_bank(
                tasks_dir=tasks, annotations_dir=annotations, require_live=False
            )[0]


if __name__ == "__main__":
    unittest.main()
