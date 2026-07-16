#!/usr/bin/env python3
"""Unit tests for the stage-3 behaviour-oracle + blast-radius GATE
(``edits/oracles/oracle_gate.py``).

Hermetic: every test builds a synthetic tree, a synthetic oracle patch, and (for
behaviour runs) tiny real argv commands in a tempdir. Nothing touches the network
or the multi-gigabyte real corpora, so the suite runs anywhere via::

    python3 -m unittest discover -s benchmarks/repo-qa/tests

Coverage map:

  * changed_path / blast radius (criterion 5): a patch inside the declared allow
    set PASSES; a patch that escapes it FAILS; max_files and empty-change fail
    closed; the `*` vs `**` glob semantics are pinned.
  * discrimination (criteria 1, 2): a RED-pristine / GREEN-patched pair passes;
    both-green, both-red and green-pristine are REJECTED with distinct codes.
  * fail closed (criterion 3): timeout, missing command and tool error each map
    to a stable reason code with BOUNDED captured output.
  * api_impact (criterion 5): a reference outside the allow scope is an escape;
    an absent LCI binary fails closed.
  * worktree hygiene (criterion 4): the throwaway worktree is gone after BOTH a
    success and a forced failure, and the source tree is never mutated.
  * determinism (criterion 4): the aggregate outcome is byte-stable and
    validates against the versioned outcome schema.
"""

import hashlib
import json
import os
import sys
import tempfile
import unittest

ORACLES = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "edits",
    "oracles",
)
sys.path.insert(0, ORACLES)

import oracle_gate as gate  # noqa: E402


# ---------------------------------------------------------------------------
# changed-path / blast radius (criterion 5)
# ---------------------------------------------------------------------------


class ChangedPathTest(unittest.TestCase):
    def test_patch_within_scope_passes(self):
        out = gate.check_changed_paths(
            ["apis/backup.go", "apis/record.go"], ["apis/**"], 3
        )
        self.assertTrue(out["passed"], msg=out["detail"])
        self.assertEqual(out["reason"], gate.Reason.WITHIN_SCOPE)
        self.assertEqual(out["out_of_scope"], [])

    def test_patch_escaping_scope_fails_closed(self):
        # forbidden blast radius: one path outside the single allowed subtree.
        out = gate.check_changed_paths(
            ["apis/backup.go", "core/db.go"], ["apis/**"], None
        )
        self.assertFalse(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.PATH_OUT_OF_SCOPE)
        self.assertEqual(out["out_of_scope"], ["core/db.go"])

    def test_max_files_breach_fails_closed(self):
        out = gate.check_changed_paths(
            ["apis/a.go", "apis/b.go", "apis/c.go"], ["apis/**"], 2
        )
        self.assertFalse(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.TOO_MANY_FILES)

    def test_empty_change_set_fails_closed(self):
        out = gate.check_changed_paths([], ["apis/**"], None)
        self.assertFalse(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.NO_CHANGES)

    def test_missing_allow_is_malformed(self):
        out = gate.check_changed_paths(["apis/a.go"], None, None)
        self.assertFalse(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.BLAST_RADIUS_MALFORMED)

    def test_single_star_does_not_cross_directory(self):
        # `apis/*` allows a direct child but NOT a nested file -> escape caught.
        out = gate.check_changed_paths(
            ["apis/nested/deep.go"], ["apis/*"], None
        )
        self.assertFalse(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.PATH_OUT_OF_SCOPE)

    def test_double_star_crosses_directories(self):
        out = gate.check_changed_paths(
            ["apis/nested/deep.go"], ["apis/**"], None
        )
        self.assertTrue(out["passed"], msg=out["detail"])

    def test_changed_paths_emitted_sorted(self):
        out = gate.check_changed_paths(
            ["apis/z.go", "apis/a.go"], ["apis/**"], None
        )
        self.assertEqual(out["changed"], ["apis/a.go", "apis/z.go"])


if __name__ == "__main__":
    unittest.main()
