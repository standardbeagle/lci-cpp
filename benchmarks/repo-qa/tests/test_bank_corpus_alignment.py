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
import unittest

BENCH_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BANK_DIRS = (
    os.path.join(BENCH_ROOT, "exploration", "tasks"),
    os.path.join(BENCH_ROOT, "edits", "tasks"),
)
CORPORA_PATH = os.path.join(BENCH_ROOT, "exploration", "corpora.json")
CORPUS_ROOT = os.path.join(BENCH_ROOT, ".work", "exploration")

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


if __name__ == "__main__":
    unittest.main()
