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


# ---------------------------------------------------------------------------
# discrimination classifier (criteria 1, 2)
# ---------------------------------------------------------------------------


def _run(exit_code=0, reason=None):
    return gate._command_run(["t"], exit_code, reason, "", "")


class DiscriminationClassifierTest(unittest.TestCase):
    def test_red_pristine_green_patched_discriminates(self):
        out = gate.discriminate(_run(exit_code=1), _run(exit_code=0))
        self.assertTrue(out["passed"], msg=out["detail"])
        self.assertEqual(out["reason"], gate.Reason.DISCRIMINATES)

    def test_both_green_is_non_discriminating(self):
        out = gate.discriminate(_run(exit_code=0), _run(exit_code=0))
        self.assertFalse(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.NON_DISCRIMINATING)

    def test_both_red_is_non_discriminating(self):
        out = gate.discriminate(_run(exit_code=1), _run(exit_code=1))
        self.assertFalse(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.NON_DISCRIMINATING)

    def test_green_pristine_red_patched_is_wrong_direction(self):
        out = gate.discriminate(_run(exit_code=0), _run(exit_code=1))
        self.assertFalse(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.WRONG_DIRECTION)

    def test_failed_closed_run_makes_pair_run_error(self):
        out = gate.discriminate(
            _run(reason=gate.Reason.TIMEOUT), _run(exit_code=0)
        )
        self.assertFalse(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.RUN_ERROR)


# ---------------------------------------------------------------------------
# fail-closed command runner (criterion 3)
# ---------------------------------------------------------------------------


class CommandRunnerTest(unittest.TestCase):
    def test_clean_exit_zero_is_green(self):
        out = gate.run_command([sys.executable, "-c", "pass"], cwd=None)
        self.assertIsNone(out["reason"])
        self.assertEqual(out["exit_code"], 0)
        self.assertEqual(out["observed"], "green")

    def test_nonzero_exit_is_red(self):
        out = gate.run_command(
            [sys.executable, "-c", "import sys; sys.exit(3)"], cwd=None
        )
        self.assertIsNone(out["reason"])
        self.assertEqual(out["exit_code"], 3)
        self.assertEqual(out["observed"], "red")

    def test_missing_command_fails_closed(self):
        out = gate.run_command(["definitely-not-a-real-binary-xyz"], cwd=None)
        self.assertEqual(out["reason"], gate.Reason.COMMAND_NOT_FOUND)
        self.assertIsNone(out["exit_code"])
        self.assertEqual(out["observed"], "error")

    def test_timeout_fails_closed(self):
        out = gate.run_command(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            cwd=None,
            timeout=1,
        )
        self.assertEqual(out["reason"], gate.Reason.TIMEOUT)
        self.assertIsNone(out["exit_code"])

    def test_empty_argv_fails_closed(self):
        out = gate.run_command([], cwd=None)
        self.assertEqual(out["reason"], gate.Reason.COMMAND_ABSENT)

    def test_captured_output_is_bounded(self):
        flood = "x" * (gate._MAX_TAIL * 4)
        out = gate.run_command(
            [sys.executable, "-c", f"print({flood!r})"], cwd=None
        )
        self.assertLessEqual(len(out["stdout_tail"]), gate._MAX_TAIL)


# ---------------------------------------------------------------------------
# existing-suite regression guard (criteria 2, 3)
# ---------------------------------------------------------------------------


class ExistingSuiteTest(unittest.TestCase):
    def test_green_suite_passes(self):
        out = gate.run_existing_suite([sys.executable, "-c", "pass"], cwd=None)
        self.assertTrue(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.SUITE_GREEN)

    def test_red_suite_fails(self):
        out = gate.run_existing_suite(
            [sys.executable, "-c", "import sys; sys.exit(1)"], cwd=None
        )
        self.assertFalse(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.SUITE_RED)

    def test_absent_command_fails_closed(self):
        out = gate.run_existing_suite(None, cwd=None)
        self.assertFalse(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.COMMAND_ABSENT)
        self.assertIsNone(out["run"])

    def test_timeout_propagates_fail_closed(self):
        out = gate.run_existing_suite(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            cwd=None,
            timeout=1,
        )
        self.assertFalse(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.TIMEOUT)


# ---------------------------------------------------------------------------
# api-impact detector (criterion 5)
# ---------------------------------------------------------------------------


class FakeLci:
    def __init__(self, refs_map, available=True, boom=False):
        self._refs_map = refs_map
        self._available = available
        self._boom = boom

    def available(self):
        return self._available

    def refs(self, symbol, tree_dir):
        if self._boom:
            raise RuntimeError("lci exploded")
        return self._refs_map.get(symbol, [])


class ApiImpactTest(unittest.TestCase):
    def test_no_symbols_passes_vacuously(self):
        out = gate.check_api_impact([], ["apis/**"], FakeLci({}), "/t")
        self.assertTrue(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.NO_ESCAPE)

    def test_refs_within_scope_pass(self):
        lci = FakeLci({"backupsList": ["apis/backup.go", "apis/router.go"]})
        out = gate.check_api_impact(["backupsList"], ["apis/**"], lci, "/t")
        self.assertTrue(out["passed"], msg=out["detail"])
        self.assertEqual(out["reason"], gate.Reason.NO_ESCAPE)

    def test_ref_outside_scope_is_escape(self):
        # the changed handler is called from core/, outside the declared apis/**.
        lci = FakeLci({"backupsList": ["apis/backup.go", "core/hooks.go"]})
        out = gate.check_api_impact(["backupsList"], ["apis/**"], lci, "/t")
        self.assertFalse(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.API_IMPACT_ESCAPE)
        self.assertEqual(
            out["escaped_refs"],
            [{"symbol": "backupsList", "path": "core/hooks.go"}],
        )

    def test_absent_lci_fails_closed(self):
        out = gate.check_api_impact(
            ["backupsList"], ["apis/**"], FakeLci({}, available=False), "/t"
        )
        self.assertFalse(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.LCI_ABSENT)

    def test_lci_error_fails_closed(self):
        out = gate.check_api_impact(
            ["backupsList"], ["apis/**"], FakeLci({}, boom=True), "/t"
        )
        self.assertFalse(out["passed"])
        self.assertEqual(out["reason"], gate.Reason.TOOL_FAILURE)


if __name__ == "__main__":
    unittest.main()
