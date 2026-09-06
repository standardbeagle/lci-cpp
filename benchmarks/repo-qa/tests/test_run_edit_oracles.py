#!/usr/bin/env python3
"""Unit tests for the run_edit_oracles driver (scripts/run_edit_oracles.py).

Hermetic: a synthetic mini-corpus (tempdir manifest + tree), a temp edits root
with one sidecar patch, and shell-free probes (``{python} -c ...``). The driver
is exercised through main(argv), never via subprocess.
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

SCRIPTS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"
)
ORACLES = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "edits",
    "oracles",
)
sys.path.insert(0, SCRIPTS)
sys.path.insert(0, ORACLES)

import oracle_gate as gate  # noqa: E402
import run_edit_oracles as driver  # noqa: E402


class DriverTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        root = self._tmp.name

        # temp edits root: tasks dir + sidecar patch
        self.edits_root = os.path.join(root, "edits")
        self.tasks_dir = os.path.join(self.edits_root, "tasks")
        self.patches_dir = os.path.join(self.edits_root, "patches", "pocketbase")
        os.makedirs(self.tasks_dir)
        os.makedirs(self.patches_dir)
        self._orig_edits_root = gate.EDITS_ROOT
        gate.EDITS_ROOT = self.edits_root
        self.addCleanup(setattr, gate, "EDITS_ROOT", self._orig_edits_root)

        # synthetic mini-corpus: manifest + tree with one OLD file
        self.corpus_root = os.path.join(root, "corpora")
        seed_dir = os.path.join(self.corpus_root, "pocketbase", "seed-7")
        tree = os.path.join(seed_dir, "tree")
        os.makedirs(os.path.join(tree, "apis"))
        with open(os.path.join(tree, "apis", "handler.go"), "w") as fh:
            fh.write("OLD handler body\n")
        with open(os.path.join(seed_dir, "manifest.json"), "w") as fh:
            json.dump(
                {
                    "schema": "exploration_corpus_manifest_v1",
                    "corpus_id": "pocketbase",
                    "seed": 7,
                    "path_map": {},
                    "decoys": [],
                    "status": "ready",
                },
                fh,
            )

        self.task = {
            "schema": "edit_task_v1",
            "id": "pb-mini-1",
            "corpus": "pocketbase",
            "manifest_ref": {"corpus_id": "pocketbase", "seed": 7},
            "behavior": {
                "command": [
                    "{python}", "-c",
                    "import sys; sys.exit(0 if 'NEW' in "
                    "open('apis/handler.go').read() else 1)",
                ]
            },
            "blast_radius": {"allow": ["apis/**"], "max_files": 1},
            "oracle_patch": {
                "path": "patches/pocketbase/pb-mini-1.json",
                "format": "edit_oracle_patch_v1",
            },
            "existing_suite": {"command": ["{python}", "-c", "pass"]},
        }
        with open(os.path.join(self.tasks_dir, "pb-mini-1.json"), "w") as fh:
            json.dump(self.task, fh)
        self.evidence_dir = os.path.join(root, "evidence")

    def _write_patch(self, body):
        with open(
            os.path.join(self.patches_dir, "pb-mini-1.json"), "w"
        ) as fh:
            json.dump(
                {
                    "schema": "edit_oracle_patch_v1",
                    "task_id": "pb-mini-1",
                    "files": {"apis/handler.go": body},
                },
                fh,
            )

    def _run(self, argv):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            code = driver.main(argv)
        return code, stdout.getvalue()

    def _argv(self, *extra):
        return [
            "--task-id", "pb-mini-1",
            "--tasks-dir", self.tasks_dir,
            "--corpus-root", self.corpus_root,
            *extra,
        ]

    def test_discrimination_only_exits_zero_when_probe_flips(self):
        self._write_patch("NEW handler body\n")
        code, out = self._run(self._argv("--discrimination-only"))
        self.assertEqual(code, 0, msg=out)
        outcome = json.loads(out)
        self.assertEqual(
            outcome["discrimination"]["reason"], "DISCRIMINATES"
        )
        self.assertTrue(outcome["existing_suite"]["passed"])

    def test_discrimination_only_exits_one_when_probe_does_not_flip(self):
        # The patch fails to introduce the marker: red on both trees.
        self._write_patch("STILL OLD handler body\n")
        code, out = self._run(self._argv("--discrimination-only"))
        self.assertEqual(code, 1, msg=out)
        outcome = json.loads(out)
        self.assertEqual(
            outcome["discrimination"]["reason"], "NON_DISCRIMINATING"
        )

    def test_evidence_file_written_where_asked(self):
        self._write_patch("NEW handler body\n")
        code, _out = self._run(
            self._argv("--discrimination-only", "--evidence-dir",
                       self.evidence_dir)
        )
        self.assertEqual(code, 0)
        evidence = os.path.join(self.evidence_dir, "pb-mini-1.oracle.json")
        self.assertTrue(os.path.isfile(evidence))
        with open(evidence) as fh:
            recorded = json.load(fh)
        self.assertEqual(recorded["task_id"], "pb-mini-1")
        self.assertEqual(recorded["schema"], "oracle_gate_v1")

    def test_no_evidence_dir_writes_no_file(self):
        self._write_patch("NEW handler body\n")
        code, _out = self._run(self._argv("--discrimination-only"))
        self.assertEqual(code, 0)
        self.assertFalse(os.path.exists(self.evidence_dir))

    # ---- --all sweep mode (M5 close-out) --------------------------------
    def _argv_all(self, *extra):
        return [
            "--all",
            "--tasks-dir", self.tasks_dir,
            "--corpus-root", self.corpus_root,
            *extra,
        ]

    def test_all_prints_one_line_per_task_and_exits_zero(self):
        self._write_patch("NEW handler body\n")
        code, out = self._run(self._argv_all("--discrimination-only"))
        self.assertEqual(code, 0, msg=out)
        self.assertEqual(
            out.strip().splitlines(), ["pb-mini-1 DISCRIMINATES True"]
        )

    def test_all_exits_one_when_any_task_does_not_discriminate(self):
        # The patch fails to introduce the marker: the sweep must report the
        # real verdict and fail, never swallow it.
        self._write_patch("STILL OLD handler body\n")
        code, out = self._run(self._argv_all("--discrimination-only"))
        self.assertEqual(code, 1, msg=out)
        self.assertEqual(
            out.strip().splitlines(), ["pb-mini-1 NON_DISCRIMINATING True"]
        )


if __name__ == "__main__":
    unittest.main()
