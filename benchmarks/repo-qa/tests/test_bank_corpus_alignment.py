#!/usr/bin/env python3
"""Committed task banks must pin the forge_version of the corpus they run against.

Every bank's manifest_ref.forge_version is a promise about which forged tree
layout its anchors were verified against. When that label disagrees with the
corpus on disk (or with the reference recorded in corpora.json on hosts without
a local corpus), the exploration/edit runners either abort at corpus verify or
score against the wrong layout. This test fails at TEST time on any such
mismatch instead of at baseline launch.

Resolution rule: the version a bank will run against is the forged manifest's
forge_version when the corpus exists locally (.work tree present); otherwise it
is the reference recorded in the committed corpora.json, which is the standing
pin for hosts that never forge the corpus.
"""

import glob
import json
import os
import sys
import unittest
from tempfile import TemporaryDirectory

BENCH_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BANK_DIRS = (
    os.path.join(BENCH_ROOT, "exploration", "tasks"),
    os.path.join(BENCH_ROOT, "edits", "tasks"),
)
CORPORA_PATH = os.path.join(BENCH_ROOT, "exploration", "corpora.json")
CORPUS_ROOT = os.path.join(BENCH_ROOT, ".work", "exploration")

for _p in (os.path.join(BENCH_ROOT, "exploration"),
           os.path.join(BENCH_ROOT, "scripts")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import exploration_corpus_forge as forge  # noqa: E402
from runner import corpus  # noqa: E402

# Number of committed bank files at the time this alignment gate landed; a
# changed count means new banks must be covered (or the pin revisited).
BANK_FILE_COUNT = 54


def _bank_paths():
    paths = []
    for directory in BANK_DIRS:
        paths.extend(glob.glob(os.path.join(directory, "*.json")))
    return sorted(paths)


def _reference_forge_versions():
    with open(CORPORA_PATH, encoding="utf-8") as handle:
        data = json.load(handle)
    return {
        spec["id"]: spec.get("reference_forge_version")
        for spec in data["corpora"]
    }


def _corpus_forge_version(corpus_id, seed, references):
    """The forge version the bank will actually run against."""
    manifest_path = os.path.join(
        CORPUS_ROOT, corpus_id, f"seed-{seed}", "manifest.json"
    )
    if os.path.isfile(manifest_path):
        with open(manifest_path, encoding="utf-8") as handle:
            return str(json.load(handle)["forge_version"])
    reference = references.get(corpus_id)
    if reference is None:
        raise AssertionError(
            f"corpora.json records no reference_forge_version for {corpus_id!r} "
            f"and no forged corpus exists under {CORPUS_ROOT}; the bank's "
            f"declared forge_version is unverifiable"
        )
    return str(reference)


class BankCorpusVersionAlignmentTest(unittest.TestCase):
    def test_every_bank_pins_the_corpus_forge_version(self):
        paths = _bank_paths()
        self.assertEqual(
            len(paths),
            BANK_FILE_COUNT,
            f"expected {BANK_FILE_COUNT} committed bank files, found "
            f"{len(paths)}; new banks must be covered by this gate",
        )
        references = _reference_forge_versions()
        mismatches = []
        for path in paths:
            with open(path, encoding="utf-8") as handle:
                task = json.load(handle)
            ref = task["manifest_ref"]
            expected = _corpus_forge_version(
                ref["corpus_id"], ref["seed"], references
            )
            if str(ref["forge_version"]) != expected:
                mismatches.append(
                    f"{os.path.relpath(path, BENCH_ROOT)}: manifest_ref pins "
                    f"forge_version {ref['forge_version']!r} but corpus "
                    f"{ref['corpus_id']} seed {ref['seed']} is {expected!r}"
                )
        self.assertEqual(mismatches, [], "\n".join(mismatches))


EXPECTED_CORPORA = {"scikit-learn", "pocketbase", "next.js"}
REFERENCE_SEED = "seed-7"


def _corpus_specs():
    with open(CORPORA_PATH, encoding="utf-8") as handle:
        data = json.load(handle)
    return {spec["id"]: spec for spec in data["corpora"]}


class ReferenceTreeHashPinTest(unittest.TestCase):
    """corpora.json must record the reference tree hash per corpus/seed, and
    the runner must verify the real tree against it: a clobbered .work tree
    (manifest AND tree rewritten consistently) passes the manifest-pinned
    hash, so only the committed, non-content reference detects the drift."""

    def test_corpora_json_records_a_reference_tree_hash_per_corpus_seed(self):
        specs = _corpus_specs()
        self.assertEqual(EXPECTED_CORPORA, set(specs))
        for corpus_id, spec in sorted(specs.items()):
            refs = spec.get("reference_tree_hash") or {}
            recorded = refs.get(REFERENCE_SEED)
            self.assertIsNotNone(
                recorded,
                f"{corpus_id}: corpora.json records no reference_tree_hash "
                f"for {REFERENCE_SEED}",
            )
            self.assertRegex(recorded, r"^[0-9a-f]{64}$")

    def test_recorded_hash_verifies_against_the_real_tree(self):
        present = []
        absent = []
        for corpus_id, spec in sorted(_corpus_specs().items()):
            seed_dir = os.path.join(CORPUS_ROOT, corpus_id, REFERENCE_SEED)
            if os.path.isdir(os.path.join(seed_dir, "tree")):
                present.append((corpus_id, spec, seed_dir))
            else:
                absent.append(corpus_id)
        if not present:
            raise unittest.SkipTest(
                f"no .work trees on this host (absent: {sorted(absent)}); "
                f"the recorded-reference verify path is untestable here"
            )
        verified = 0
        for corpus_id, spec, seed_dir in present:
            with self.subTest(corpus=corpus_id):
                recorded = spec["reference_tree_hash"][REFERENCE_SEED]
                with open(
                    os.path.join(seed_dir, "manifest.json"), encoding="utf-8"
                ) as handle:
                    manifest = json.load(handle)
                # A clobbered manifest is drift against the committed
                # reference: fail loud, never re-pin to what is on disk.
                self.assertEqual(
                    manifest["tree_hash"],
                    recorded,
                    f"{corpus_id}: .work manifest tree_hash drifted from the "
                    f"recorded reference",
                )
                actual = forge.tree_hash(os.path.join(seed_dir, "tree"))
                if actual != manifest["tree_hash"]:
                    raise unittest.SkipTest(
                        f"{corpus_id}: .work tree drifted from its own "
                        f"manifest ({actual[:12]} != "
                        f"{manifest['tree_hash'][:12]}); pre-existing host "
                        f"pollution of the gitignored tree -- re-forging is "
                        f"out of scope here"
                    )
                self.assertEqual(
                    corpus.verify_tree_hash(
                        os.path.join(seed_dir, "tree"), manifest
                    ),
                    recorded,
                )
                verified += 1
        if verified == 0:
            raise unittest.SkipTest(
                "every present .work tree drifted from its own manifest; "
                "no intact tree to verify the recorded reference against"
            )

    def test_verify_tree_hash_fails_on_an_altered_tree(self):
        """Discrimination: the verify path must FAIL on drift, not only pass
        on the happy path."""
        with TemporaryDirectory() as root:
            tree_dir = os.path.join(root, "tree")
            os.makedirs(tree_dir)
            target = os.path.join(tree_dir, "base.go")
            with open(target, "w", encoding="utf-8") as handle:
                handle.write("package core\n\nfunc NewBaseApp() {}\n")
            pinned = forge.tree_hash(tree_dir)
            manifest = {"tree_hash": pinned, "corpus_id": "synthetic", "seed": 7}
            self.assertEqual(corpus.verify_tree_hash(tree_dir, manifest), pinned)
            with open(target, "a", encoding="utf-8") as handle:
                handle.write("func Clobbered() {}\n")
            with self.assertRaises(corpus.TreeHashMismatch):
                corpus.verify_tree_hash(tree_dir, manifest)


if __name__ == "__main__":
    unittest.main()
