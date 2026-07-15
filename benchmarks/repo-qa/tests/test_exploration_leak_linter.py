#!/usr/bin/env python3
"""Contract tests for the stage-1 exploration prompt leak linter.

The linter derives forbidden material from each task's FINAL adjudicated
evidence set plus its referenced S1 translation manifest (never a hand-written
denylist) and checks the PROMPT ONLY. These tests pin one positive fixture per
leak category -- including a decoy/original-path translation case that only the
manifest can reveal -- assert the linter redacts every match, and assert the
committed stage-1 task bank passes cleanly.
"""

import os
import sys
import unittest

SCRIPTS_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"
)
sys.path.insert(0, SCRIPTS_DIR)

import lint_exploration_leaks as linter  # noqa: E402


def _task(prompt, evidence):
    """A minimal task object; the linter only reads prompt + evidence."""
    return {
        "schema": "exploration_task_v1",
        "id": "fx-task",
        "corpus": "next.js",
        "prompt": prompt,
        "evidence": evidence,
    }


def _anchor(path, identifiers):
    return {"path": path, "lines": [1], "target_identifiers": list(identifiers)}


class NormalizeTokensTests(unittest.TestCase):
    def test_splits_camel_snake_kebab_and_separators_alike(self):
        want = ["load", "manifest"]
        for form in ("loadManifest", "load_manifest", "load-manifest",
                     "LoadManifest", "load manifest", "load/manifest"):
            self.assertEqual(linter.normalize_tokens(form), want, form)


class LeakCategoryTests(unittest.TestCase):
    def _cats(self, leaks):
        return {cat for (_id, cat, _red) in leaks}

    def test_full_oracle_path_leak(self):
        anchor = _anchor("packages/next/src/server/load-manifest.ts", ["loadManifest"])
        task = _task(
            "Where is packages/next/src/server/load-manifest.ts wired in?",
            [anchor],
        )
        self.assertIn("oracle-path", self._cats(linter.find_leaks(task)))

    def test_basename_leak(self):
        anchor = _anchor("packages/next/src/server/load-manifest.ts", ["loadManifest"])
        task = _task("Which function lives in load-manifest.ts on disk?", [anchor])
        self.assertIn("basename", self._cats(linter.find_leaks(task)))

    def test_path_segment_sequence_leak(self):
        anchor = _anchor("packages/next/src/server/load-manifest.ts", ["loadManifest"])
        task = _task(
            "Trace the server/load-manifest boundary for the descriptor read.",
            [anchor],
        )
        self.assertIn("path-segments", self._cats(linter.find_leaks(task)))

    def test_target_identifier_leak(self):
        anchor = _anchor("packages/next/src/server/load-manifest.ts", ["loadManifest"])
        task = _task("Where does loadManifest cache the descriptor?", [anchor])
        self.assertIn("target-identifier", self._cats(linter.find_leaks(task)))

    def test_reformatted_identifier_and_basename_leak(self):
        anchor = _anchor("pkg/foo/bar-baz.ts", ["computeScore"])
        # snake-cased identifier + underscored basename: trivially reformatted.
        task = _task(
            "How does compute_score feed the bar_baz.ts consumer?",
            [anchor],
        )
        cats = self._cats(linter.find_leaks(task))
        self.assertIn("target-identifier", cats)
        self.assertIn("basename", cats)

    def test_concatenated_identifier_leak(self):
        anchor = _anchor("pkg/foo/thing.ts", ["computeScore"])
        task = _task("Where is computescore invoked?", [anchor])
        self.assertIn("target-identifier", self._cats(linter.find_leaks(task)))

    def test_decoy_original_path_translation_leak(self):
        # The oracle anchor is a MUTATED path; the prompt names the ORIGINAL
        # pre-mutation path, which only the S1 manifest can translate to it.
        manifest = {
            "path_map": {
                "src/original/donor-widget.ts": "src/mutated/twin-abcd.ts",
            },
            "decoys": [],
        }
        anchor = _anchor("src/mutated/twin-abcd.ts", ["twinAbcd"])
        task = _task(
            "Start from src/original/donor-widget.ts and trace outward.",
            [anchor],
        )
        cats = self._cats(linter.find_leaks(task, manifest))
        self.assertIn("translated-path", cats)
        # Without the manifest the original path is invisible -> no leak.
        self.assertNotIn(
            "translated-path", self._cats(linter.find_leaks(task, None))
        )

    def test_decoy_derived_from_translation_leak(self):
        manifest = {
            "path_map": {},
            "decoys": [
                {
                    "path": "pkg/dead/twin-xyz.go",
                    "derived_from": "pkg/live/real-broker.go",
                }
            ],
        }
        anchor = _anchor("pkg/dead/twin-xyz.go", ["BrokerXyz"])
        task = _task("Inspect pkg/live/real-broker.go behaviour.", [anchor])
        self.assertIn(
            "translated-path", self._cats(linter.find_leaks(task, manifest))
        )


class NoFalsePositiveTests(unittest.TestCase):
    def test_generic_architecture_words_not_flagged(self):
        anchor = _anchor(
            "packages/app/src/server/tools/client-registry.ts", ["registerClient"]
        )
        task = _task(
            "The server exposes tools to a client over a socket; where is that "
            "handshake set up?",
            [anchor],
        )
        self.assertEqual(linter.find_leaks(task), [])


class RedactionTests(unittest.TestCase):
    def test_redacted_form_never_emits_full_oracle(self):
        secret_path = "packages/next/src/server/load-manifest.ts"
        secret_id = "loadManifest"
        anchor = _anchor(secret_path, [secret_id])
        task = _task(
            f"Find {secret_path} and the {secret_id} entry point.", [anchor]
        )
        leaks = linter.find_leaks(task)
        self.assertTrue(leaks)
        for (_id, _cat, redacted) in leaks:
            self.assertNotIn(secret_path, redacted)
            self.assertNotIn(secret_id, redacted)
            self.assertNotIn("load-manifest", redacted)


class RealBankTests(unittest.TestCase):
    def test_committed_bank_passes_validator_then_linter(self):
        problems, _summary, leaks = linter.lint_bank()
        self.assertEqual(problems, [], f"validator pre-check failed: {problems}")
        self.assertEqual(leaks, [], f"linter flagged committed bank: {leaks}")

    def test_validation_runs_before_leak_checks(self):
        # A malformed task (schema-invalid) must surface as a validator problem
        # and short-circuit before any leak scan.
        import tempfile
        import json
        with tempfile.TemporaryDirectory() as tmp:
            tasks_dir = os.path.join(tmp, "tasks")
            os.makedirs(tasks_dir)
            with open(os.path.join(tasks_dir, "bad.json"), "w") as fh:
                json.dump({"schema": "exploration_task_v1", "id": "bad"}, fh)
            problems, _summary, leaks = linter.lint_bank(
                tasks_dir=tasks_dir,
                annotations_dir=os.path.join(tmp, "annotations"),
            )
            self.assertTrue(problems)
            self.assertEqual(leaks, [])


if __name__ == "__main__":
    unittest.main()
