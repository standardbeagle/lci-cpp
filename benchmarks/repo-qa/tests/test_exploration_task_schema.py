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
import json
import os
import sys
import tempfile
import unittest

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
        }
        self._annotation("ann-a")
        self._annotation("ann-b")
        self._flush_task()

    def _annotation(self, annotator, evidence=None):
        record = {
            "schema": "exploration_annotation_v1",
            "task_id": self.task["id"],
            "annotator": annotator,
            "evidence": copy.deepcopy(
                evidence if evidence is not None else self.task["evidence"]
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
        return vet.validate_bank(
            tasks_dir=self.tasks_dir,
            annotations_dir=self.annotations_dir,
            schema_path=vet.DEFAULT_SCHEMA_PATH,
            corpora_path=vet.DEFAULT_CORPORA_PATH,
            corpus_root=self.corpus_root,
            require_live=require_live,
        )[0]


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


class RealTaskBankTest(unittest.TestCase):
    """The committed bank must be schema+structurally valid without a live tree."""

    def test_bank_size_and_corpora_coverage(self):
        problems, summary = vet.validate_bank(require_live=False)
        self.assertEqual(problems, [], msg="\n".join(problems))
        self.assertGreaterEqual(summary["tasks"], 25)
        self.assertLessEqual(summary["tasks"], 40)
        self.assertEqual(
            set(summary["per_corpus"]),
            {"scikit-learn", "pocketbase", "next.js"},
        )
        for corpus, count in summary["per_corpus"].items():
            self.assertGreater(count, 0, msg=f"{corpus} has no tasks")


if __name__ == "__main__":
    unittest.main()
