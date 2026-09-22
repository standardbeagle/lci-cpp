"""Contract tests for the opencode arm adapter (stage-2 pilot).

Hermetic: no opencode binary, no provider, no corpus. The subject is the
translation layer between opencode's vocabulary and the one the arms and the
isolation gate are written in, plus the arm configs those arms launch under.

The bullets pinned here, one acceptance criterion each:
  * the two arms are DISJOINT by construction and neither is a superset of the
    other (bench-harness-oracle-independence rule 12) -- asserted in BOTH
    directions, so a drift either way fails;
  * an unmapped tool or argument passes through VERBATIM, so the gate rejects
    it; a translation layer that silently canonicalised an unknown name would
    convert a disjointness violation into a clean run;
  * the normalisation table matches what opencode really emits, and the gate
    accepts a real treatment/baseline call sequence end to end.
"""

import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH_ROOT = os.path.dirname(HERE)
EXPLORATION_ROOT = os.path.join(BENCH_ROOT, "exploration")
SCRIPTS = os.path.join(BENCH_ROOT, "scripts")
for _p in (EXPLORATION_ROOT, SCRIPTS):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from runner import gate  # noqa: E402
from runner.adapter import (  # noqa: E402
    OPENCODE_NATIVE_TOOL_IDS, canonical_arguments, canonical_tool_name,
    opencode_tools_map, opencode_workspace_config,
)
from runner.toolsets import BASELINE_TOOLS, TREATMENT_TOOLS  # noqa: E402

# Registering the LCI server now means ASKING it what it serves, so any test
# that builds a treatment config needs the real binary.
LCI_BIN = os.path.join(os.path.dirname(os.path.dirname(BENCH_ROOT)),
                       "build", "release", "src", "lci")


def require_lci(case):
    if not os.path.isfile(LCI_BIN):
        case.skipTest(f"lci binary not built at {LCI_BIN}")
    return LCI_BIN


class ArmDisjointnessTest(unittest.TestCase):
    """Rule 12: the treatment must not be able to reach the baseline's
    mechanism, nor the baseline the treatment's."""

    def test_treatment_denies_the_baselines_lexical_tools(self):
        tools = opencode_tools_map(TREATMENT_TOOLS)
        self.assertIs(tools["grep"], False)
        self.assertIs(tools["glob"], False)

    def test_baseline_grants_the_lexical_tools_it_is_defined_by(self):
        tools = opencode_tools_map(BASELINE_TOOLS)
        self.assertNotIn("grep", tools, "grep must stay enabled for the baseline")
        self.assertNotIn("glob", tools, "glob must stay enabled for the baseline")

    def test_baseline_never_registers_the_lci_server(self):
        config = opencode_workspace_config(BASELINE_TOOLS)
        self.assertNotIn("lci", config["mcp"])

    def test_treatment_registers_the_lci_server_at_the_given_binary(self):
        lci = require_lci(self)
        config = opencode_workspace_config(TREATMENT_TOOLS, lci)
        self.assertEqual(config["mcp"]["lci"]["command"], [lci, "mcp"])
        self.assertTrue(config["mcp"]["lci"]["enabled"])

    def test_treatment_without_a_binary_refuses_to_launch(self):
        with self.assertRaises(ValueError):
            opencode_workspace_config(TREATMENT_TOOLS, None)

    def test_neither_arm_is_a_superset_of_the_other(self):
        def granted(allowlist):
            denied = opencode_tools_map(allowlist)
            return {t for t in OPENCODE_NATIVE_TOOL_IDS if t not in denied}

        treatment, baseline = granted(TREATMENT_TOOLS), granted(BASELINE_TOOLS)
        self.assertTrue(baseline - treatment, "baseline must hold a tool the treatment lacks")
        lci = require_lci(self)
        self.assertIn("lci", opencode_workspace_config(TREATMENT_TOOLS, lci)["mcp"])
        self.assertNotIn("lci", opencode_workspace_config(BASELINE_TOOLS)["mcp"])

    def test_both_arms_deny_every_escape_hatch(self):
        for allowlist in (TREATMENT_TOOLS, BASELINE_TOOLS):
            tools = opencode_tools_map(allowlist)
            for escape in ("bash", "edit", "write", "apply_patch", "task", "websearch"):
                self.assertIs(tools[escape], False, f"{escape} must be denied")

    def test_an_allowlist_naming_an_unenforceable_tool_refuses_to_launch(self):
        with self.assertRaises(ValueError):
            opencode_tools_map(("Read", "NotebookEdit"))


class NormalisationTest(unittest.TestCase):
    """The table is derived from recorded streams; these pin what it must do
    with names and arguments it does NOT know."""

    def test_native_and_mcp_names_reach_the_gate_vocabulary(self):
        self.assertEqual(canonical_tool_name("read"), "Read")
        self.assertEqual(canonical_tool_name("grep"), "Grep")
        self.assertEqual(canonical_tool_name("glob"), "Glob")
        self.assertEqual(canonical_tool_name("lci_search"), "mcp__lci__search")
        self.assertEqual(canonical_tool_name("lci_browse_file"), "mcp__lci__browse_file")

    def test_an_unmapped_tool_passes_through_so_the_gate_can_reject_it(self):
        for native in ("bash", "task", "websearch", "codesearch"):
            self.assertEqual(canonical_tool_name(native), native)
            self.assertNotIn(canonical_tool_name(native), TREATMENT_TOOLS)
            self.assertNotIn(canonical_tool_name(native), BASELINE_TOOLS)

    def test_recorded_argument_spellings_are_renamed(self):
        self.assertEqual(canonical_arguments("Read", {"filePath": "a.go", "limit": 5}),
                         {"file_path": "a.go", "limit": 5})
        self.assertEqual(canonical_arguments("Grep", {"pattern": "x", "include": "*.go"}),
                         {"pattern": "x", "glob": "*.go"})

    def test_an_unmapped_argument_passes_through_so_the_gate_can_reject_it(self):
        # grep.flags appeared once in 262 recorded calls and has no canonical
        # equivalent: it must stay unknown, not be guessed at.
        self.assertEqual(canonical_arguments("Grep", {"pattern": "x", "flags": "-i"}),
                         {"pattern": "x", "flags": "-i"})


class GateIntegrationTest(unittest.TestCase):
    """End to end: a normalised opencode call sequence must survive the very
    gate that scores the run, and a cross-arm call must still be caught."""

    def _calls(self, raw):
        from runner.adapter import ToolCall
        return [ToolCall(canonical_tool_name(n), canonical_arguments(canonical_tool_name(n), a))
                for n, a in raw]

    def test_a_real_treatment_sequence_is_clean(self):
        calls = self._calls([
            ("lci_search", {"pattern": "bindRecordCrudApi", "max": 30}),
            ("read", {"filePath": "apis/base.go", "offset": 1, "limit": 60}),
        ])
        self.assertEqual(gate.enforce(calls, TREATMENT_TOOLS, os.getcwd()), [])

    def test_a_real_baseline_sequence_is_clean(self):
        calls = self._calls([
            ("grep", {"pattern": "func NewRouter", "include": "*.go"}),
            ("glob", {"pattern": "apis/*.go"}),
            ("read", {"filePath": "apis/base.go"}),
        ])
        self.assertEqual(gate.enforce(calls, BASELINE_TOOLS, os.getcwd()), [])

    def test_a_baseline_reaching_for_lci_is_rejected(self):
        calls = self._calls([("lci_search", {"pattern": "x"})])
        violations = gate.enforce(calls, BASELINE_TOOLS, os.getcwd())
        self.assertEqual([v["reason"] for v in violations], ["tool_not_allowed"])

    def test_a_treatment_reaching_for_grep_is_rejected(self):
        calls = self._calls([("grep", {"pattern": "x"})])
        violations = gate.enforce(calls, TREATMENT_TOOLS, os.getcwd())
        self.assertEqual([v["reason"] for v in violations], ["tool_not_allowed"])

    def test_a_shell_escape_is_rejected_in_both_arms(self):
        calls = self._calls([("bash", {"command": "grep -r x ."})])
        for allowlist in (TREATMENT_TOOLS, BASELINE_TOOLS):
            violations = gate.enforce(calls, allowlist, os.getcwd())
            self.assertEqual([v["reason"] for v in violations], ["tool_not_allowed"])


if __name__ == "__main__":
    unittest.main()


class LciSurfaceNarrowingTest(unittest.TestCase):
    """Registering the LCI MCP server exposes its WHOLE surface, which is
    wider than the arm. The config must close that gap, or the gate rejects a
    non-allowlisted call mid-run and the cell is lost."""

    def setUp(self):
        self.LIVE = require_lci(self)

    def test_every_served_tool_outside_the_allowlist_is_denied(self):
        from runner.adapter import lci_mcp_tool_ids
        config = opencode_workspace_config(TREATMENT_TOOLS, self.LIVE)
        granted = {t[len("mcp__lci__"):] for t in TREATMENT_TOOLS
                   if t.startswith("mcp__lci__")}
        for name in lci_mcp_tool_ids(self.LIVE):
            key = f"lci_{name}"
            if name in granted:
                self.assertNotIn(key, config["tools"], f"{name} is allowlisted")
            else:
                self.assertIs(config["tools"][key], False, f"{name} must be denied")

    def test_the_observed_violation_tool_is_denied(self):
        # index_stats is what a live treatment cell actually reached for, and
        # the gate rejected the whole run for it.
        config = opencode_workspace_config(TREATMENT_TOOLS, self.LIVE)
        self.assertIs(config["tools"]["lci_index_stats"], False)

    def test_enumeration_failure_raises_rather_than_widening_the_arm(self):
        from runner.adapter import lci_mcp_tool_ids
        with self.assertRaises(Exception):
            lci_mcp_tool_ids("/bin/true")
