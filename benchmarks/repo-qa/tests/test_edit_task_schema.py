#!/usr/bin/env python3
"""Unit tests for the stage-3 convention-conformance edit task validator.

Hermetic: every live test builds a synthetic forged corpus (a tiny tree + a
manifest in the exact shape the forge emits, including a decoy) in a tempdir.
Nothing here needs the network or the multi-gigabyte real corpora, so the suite
runs anywhere via

    python3 -m unittest discover -s benchmarks/repo-qa/tests

The final section additionally validates the REAL committed task bank against the
schema and the structural rules (gate degeneracy, dual annotation, prompt leak)
WITHOUT live anchors, so a malformed committed task is caught even where the
forged tree is absent.
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

import validate_edit_tasks as vedt  # noqa: E402

CORPUS_ID = "pocketbase"
SEED = 99
FILE_REL = "core/base.go"
DECOY_REL = "core/base_dead.go"
FILE_BODY = "\n".join(
    [
        "package core",                              # 1
        "",                                          # 2
        "type BaseApp struct {",                     # 3
        "\tstore *Store",                            # 4
        "}",                                         # 5
        "",                                          # 6
        "func NewBaseApp(cfg Config) *BaseApp {",    # 7
        "\treturn &BaseApp{store: NewStore(cfg)}",   # 8
        "}",                                         # 9
        "",                                          # 10
        "type Store struct{}",                       # 11
        "",                                          # 12
        "func NewStore(cfg Config) *Store {",        # 13
        "\treturn &Store{}",                         # 14
        "}",                                         # 15
    ]
) + "\n"

CONFORMS = r"^func New\w+\("


def _pinned_commit():
    corpora = vedt.vet.load_corpora()
    return corpora[CORPUS_ID]["pinned_commit"]


def _write(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        if isinstance(obj, str):
            handle.write(obj)
        else:
            json.dump(obj, handle, indent=2)


class Fixture:
    """A self-contained, schema-valid edit task bank plus its forged corpus."""

    def __init__(self, root):
        self.root = root
        self.tasks_dir = os.path.join(root, "tasks")
        self.annotations_dir = os.path.join(root, "annotations")
        self.corpus_root = os.path.join(root, "corpus_root")
        self.commit = _pinned_commit()

        tree = os.path.join(self.corpus_root, CORPUS_ID, f"seed-{SEED}", "tree")
        self.tree_dir = tree
        _write(os.path.join(tree, FILE_REL), FILE_BODY)
        manifest = {
            "schema": "exploration_corpus_manifest_v1",
            "corpus_id": CORPUS_ID,
            "source_commit": self.commit,
            "seed": SEED,
            "forge_version": vedt.vet.forge.FORGE_VERSION,
            "path_map": {FILE_REL: FILE_REL},
            "decoys": [{"path": DECOY_REL, "derived_from": FILE_REL}],
            "tree_hash": "0" * 64,
            "status": "ready",
        }
        self.manifest = manifest
        _write(
            os.path.join(
                self.corpus_root, CORPUS_ID, f"seed-{SEED}", "manifest.json"
            ),
            manifest,
        )

        self.task = {
            "schema": "edit_task_v1",
            "id": "pb-ctor-shape",
            "corpus": CORPUS_ID,
            "category": "module-extraction-layout",
            "manifest_ref": {
                "corpus_id": CORPUS_ID,
                "source_commit": self.commit,
                "seed": SEED,
                "forge_version": vedt.vet.forge.FORGE_VERSION,
            },
            "prompt": (
                "One type in this backend is assembled inline by its callers "
                "instead of through the constructor idiom its siblings expose; "
                "add the missing constructor and route callers through it."
            ),
            "rubric": {
                "must_surface": ["the constructor idiom", "the sibling exemplars"],
                "answer_shape": "a small patch introducing the missing constructor",
            },
            "behavior": {
                "command": ["go", "build", "./..."],
                "assertion": "callers obtain the value from a New<Type> constructor",
                "discrimination": {
                    "red": "callers hand-assemble the struct literal",
                    "green": "callers obtain the value from the constructor",
                },
            },
            "convention": {
                "rule_id": "go-constructor-new-pointer",
                "statement": "A type is created through a New<Type> constructor returning a pointer.",
                "conforms_pattern": CONFORMS,
            },
            "exemplars": [
                {
                    "path": FILE_REL,
                    "lines": [7],
                    "claim": "constructs the base app via a New<Type> function",
                    "target_identifiers": ["NewBaseApp"],
                },
                {
                    "path": FILE_REL,
                    "lines": [13],
                    "claim": "constructs the store via a New<Type> function",
                    "target_identifiers": ["NewStore"],
                },
            ],
            "blast_radius": {"allow": ["core/**"], "max_files": 2},
            "adjudication": {
                "annotators": ["ann-a", "ann-b"],
                "resolved": True,
                "notes": "concurring",
            },
        }
        self._annotation("ann-a")
        self._annotation("ann-b")
        self._flush_task()

    def _ann_exemplars(self, exemplars):
        out = []
        for anchor in exemplars:
            out.append(
                {
                    "path": anchor["path"],
                    "lines": anchor["lines"],
                    "target_identifiers": anchor["target_identifiers"],
                    "conforms": True,
                }
            )
        return out

    def _annotation(self, annotator, exemplars=None, mutate=None):
        recs = self._ann_exemplars(
            exemplars if exemplars is not None else self.task["exemplars"]
        )
        if mutate:
            mutate(recs)
        record = {
            "schema": "edit_annotation_v1",
            "task_id": self.task["id"],
            "annotator": annotator,
            "exemplars": recs,
        }
        _write(
            os.path.join(
                self.annotations_dir, f"{self.task['id']}.{annotator}.json"
            ),
            record,
        )

    def _flush_task(self):
        _write(
            os.path.join(self.tasks_dir, f"{self.task['id']}.json"), self.task
        )

    def run(self, require_live=True):
        # Per-task gates, like the exploration fixture: bank-level coverage
        # gates are exercised separately (a one-task bank cannot cover all
        # four convention categories).
        return vedt.validate_task(
            self.task,
            vedt.load_schema(),
            vedt.vet.load_corpora(vedt.DEFAULT_CORPORA_PATH),
            self.annotations_dir,
            self.corpus_root,
            require_live,
        )

    def run_bank(self, require_live=True):
        return vedt.validate_bank(
            tasks_dir=self.tasks_dir,
            annotations_dir=self.annotations_dir,
            schema_path=vedt.DEFAULT_SCHEMA_PATH,
            corpora_path=vedt.DEFAULT_CORPORA_PATH,
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

    # ---- schema rejects MISSING gates (criterion 1) --------------------
    def test_missing_behavior_gate_fails(self):
        del self.fx.task["behavior"]
        self.fx._flush_task()
        self.assertTrue(any("schema violation" in p for p in self.fx.run()))

    def test_missing_convention_gate_fails(self):
        del self.fx.task["convention"]
        self.fx._flush_task()
        self.assertTrue(any("schema violation" in p for p in self.fx.run()))

    def test_missing_blast_radius_gate_fails(self):
        del self.fx.task["blast_radius"]
        self.fx._flush_task()
        self.assertTrue(any("schema violation" in p for p in self.fx.run()))

    def test_fewer_than_two_exemplars_fails(self):
        self.fx.task["exemplars"] = self.fx.task["exemplars"][:1]
        self.fx._annotation("ann-a", self.fx.task["exemplars"])
        self.fx._annotation("ann-b", self.fx.task["exemplars"])
        self.fx._flush_task()
        self.assertTrue(any("schema violation" in p for p in self.fx.run()))

    # ---- validator rejects DEGENERATE gates (criterion 1) --------------
    def test_degenerate_discrimination_fails(self):
        self.fx.task["behavior"]["discrimination"]["green"] = self.fx.task[
            "behavior"
        ]["discrimination"]["red"]
        self.fx._flush_task()
        self.assertTrue(any("discrimination is degenerate" in p for p in self.fx.run()))

    def test_degenerate_convention_pattern_fails(self):
        self.fx.task["convention"]["conforms_pattern"] = ".*"
        self.fx._flush_task()
        self.assertTrue(
            any("matches the empty string" in p for p in self.fx.run())
        )

    def test_degenerate_blast_radius_fails(self):
        self.fx.task["blast_radius"]["allow"] = ["**"]
        self.fx._flush_task()
        self.assertTrue(
            any("matches\neverything" in p or "matches everything" in p
                for p in self.fx.run())
        )

    # ---- dual annotation (criterion 3) ---------------------------------
    def test_missing_second_annotation_fails(self):
        os.remove(
            os.path.join(self.fx.annotations_dir, "pb-ctor-shape.ann-b.json")
        )
        self.assertTrue(self.fx.run())

    def test_unresolved_disagreement_fails(self):
        # ann-b anchors a different line bound and the task is not adjudicated.
        alt = copy.deepcopy(self.fx.task["exemplars"])
        alt[0]["lines"] = [7, 9]
        self.fx._annotation("ann-b", alt)
        self.fx.task["adjudication"]["resolved"] = False
        self.fx._flush_task()
        self.assertTrue(any("disagree" in p for p in self.fx.run()))

    # ---- LIVENESS discrimination + oracle independence (criterion 3) ---
    def test_exemplar_at_decoy_path_fails(self):
        # A dead/deprecated twin path must be rejected as not-live. This is the
        # broken form of the liveness oracle; the valid fixture is the good form.
        self.fx.task["exemplars"][0]["path"] = DECOY_REL
        self.fx._annotation("ann-a", self.fx.task["exemplars"])
        self.fx._annotation("ann-b", self.fx.task["exemplars"])
        self.fx._flush_task()
        self.assertTrue(any("is a DECOY" in p for p in self.fx.run()))

    def test_nonconforming_exemplar_fails(self):
        # An anchor whose bounded region does NOT match the convention pattern is
        # rejected -- proving conforms_pattern is a real oracle, not decoration.
        self.fx.task["exemplars"][0]["lines"] = [3]
        self.fx.task["exemplars"][0]["target_identifiers"] = ["BaseApp"]
        self.fx._annotation("ann-a", self.fx.task["exemplars"])
        self.fx._annotation("ann-b", self.fx.task["exemplars"])
        self.fx._flush_task()
        self.assertTrue(
            any("does not match the" in p for p in self.fx.run())
        )

    def test_non_positive_start_bound_fails(self):
        # Bank validation and the RUNTIME gate must agree on what a valid bound
        # is. conformance_gate rejects `start < 1` (ANCHOR_BOUND_STALE); the
        # validator only checked inversion and overrun, so a `[0, N]` anchor
        # passed the bank and then failed mid-drain -- a divergence that hides an
        # authoring defect until the expensive run. Checked at the verify level
        # so it holds for every caller, not only the schema-guarded one.
        import re
        for bound in ([0, 3], [0], [-1, 3]):
            with self.subTest(lines=bound):
                anchor = {
                    "path": FILE_REL,
                    "lines": bound,
                    "claim": "x",
                    "target_identifiers": ["NewBaseApp"],
                }
                problems = list(
                    vedt.verify_exemplar_live(
                        anchor, re.compile(CONFORMS, re.M),
                        self.fx.manifest, self.fx.tree_dir,
                    )
                )
                self.assertTrue(
                    any("line bound" in p for p in problems),
                    f"non-positive start {bound} accepted: {problems}",
                )

    def test_stale_line_bound_fails(self):
        self.fx.task["exemplars"][0]["lines"] = [7, 999]
        self.fx._annotation("ann-a", self.fx.task["exemplars"])
        self.fx._annotation("ann-b", self.fx.task["exemplars"])
        self.fx._flush_task()
        self.assertTrue(any("exceeds file length" in p for p in self.fx.run()))

    # ---- prompt leak (criterion 2) -------------------------------------
    def test_prompt_leak_of_identifier_fails(self):
        self.fx.task["prompt"] = (
            "Add a NewBaseApp constructor and route callers through NewStore."
        )
        self.fx._flush_task()
        self.assertTrue(any("prompt leak" in p for p in self.fx.run()))

    # ---- skipped-live discipline ---------------------------------------
    def test_absent_corpus_without_require_live_skips_live(self):
        self.fx.task["manifest_ref"]["seed"] = 12345  # no such forged tree
        self.fx._flush_task()
        self.assertEqual(self.fx.run(require_live=False), [])

    def test_absent_corpus_with_require_live_fails(self):
        self.fx.task["manifest_ref"]["seed"] = 12345
        self.fx._flush_task()
        self.assertTrue(
            any("not found" in p for p in self.fx.run(require_live=True))
        )


class BankCoverageTest(unittest.TestCase):
    """The category-coverage gate, mirroring the exploration validator's bank
    gates: a bank missing any of the four convention families measures only
    the families it happens to contain."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_bank_missing_required_categories_fails(self):
        problems = self.fx.run_bank(require_live=False)
        self.assertTrue(
            any("must cover" in p and "categor" in p for p in problems),
            msg="\n".join(problems),
        )


class RealTaskBankTest(unittest.TestCase):
    """The committed bank must be schema+structurally valid without a live tree."""

    def test_bank_size_and_coverage(self):
        problems, summary = vedt.validate_bank(require_live=False)
        self.assertEqual(problems, [], msg="\n".join(problems))
        self.assertGreaterEqual(summary["tasks"], 20)
        self.assertLessEqual(summary["tasks"], 30)
        # criterion 4: >=3 source corpora and all four convention families.
        self.assertGreaterEqual(len(summary["per_corpus"]), 3)
        self.assertEqual(
            set(summary["per_category"]), vedt.REQUIRED_CATEGORIES
        )
        for corpus, count in summary["per_corpus"].items():
            self.assertGreater(count, 0, msg=f"{corpus} has no tasks")


if __name__ == "__main__":
    unittest.main()
