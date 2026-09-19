"""Hermetic gates for bench.py's agent-level arm isolation (task
01M1W0147178Z2PZNZ6ZQQQBRR).

Found by the D5 review (task 01KXQ2V3PW91QTTQ1NTZXH7PR6): `workspace_config`
only ever ADDED the LCI MCP server for the lci* variants; it never disabled the
native opencode `grep`/`glob` tools. So every agent-level treatment cell of a
two-arm bench through bench.py actually measured "LCI in addition to grep",
contradicting the registered treatment arm in discovery/predictions.json
("LCI MCP + Read, no Grep, no Glob") and making `control_literal_string_search`
incapable of proving an LCI loss.

Review attempt 2 (this file's added criteria):

  * Both arms' tools maps are DERIVED from discovery/predictions.json
    arms.treatment.tools / arms.baseline.tools: every opencode native tool the
    arm does NOT list is denied. The registry grants the baseline only
    Glob/Grep/Read — NOT bash — so bash is denied for BOTH arms. The old
    comment claiming the registry sanctioned baseline bash was wrong and is
    corrected here and in bench.py.
  * The complete tool surface of both arms is asserted, so drift between the
    registry arm lists and the emitted config fails in BOTH directions: a
    changed registry list breaks the snapshot assertion, and a regressed
    emitter breaks the derivation assertion.
  * `run_one` must launch through `opencode_runner.isolated_environment` (plus
    the bench.py-side drops in `arm_environment`) so no host-level opencode
    config (global config, project discovery via a stale $PWD, or XDG state) can
    re-allow a tool the arm's tools map denies. It must ALSO drop
    `OPENCODE_CONFIG_DIR`: that variable re-anchors opencode's global config dir
    and bypasses XDG_CONFIG_HOME entirely, so a `"*": "allow"` from the session's
    global config merges over the arm's tools map and silently re-grants native
    grep to the treatment arm.
  * Review attempt 4: the isolation contract must not itself contaminate the
    measured copy. `isolated_environment` redirects all four XDG base dirs to
    `<dir>/.xdg-*`, so pointing it straight at the corpus workspace writes
    opencode config/state/data/cache/log files INSIDE the copy the arms answer
    from — the prior smoke's own base-arm grep matched the generated
    `.xdg-data-home/opencode/log/opencode.log`, proving the leak. `arm_environment`
    must therefore call `isolated_environment` against an EXTERNAL per-cell state
    root (opencode's writable state lands there) while re-pointing
    `OPENCODE_CONFIG` and `PWD` back INTO the workspace, so the arm still reads
    its own `opencode.json` and answers about the corpus. A cell must create no
    `.xdg-*` (or any) opencode path inside the workspace.

Verified live on 1.18.31 by the results/ discrimination smokes: ordered to call
grep, the lci arm answers TOOL_UNAVAILABLE with zero native tool calls while the
base arm completes the grep against the (now log-free) corpus copy.

Bash decision (corrected): discovery/predictions.json arms.baseline.tools is
["Glob", "Grep", "Read"] — the registry does NOT grant bash to the baseline.
arm_tools("baseline") therefore denies bash, and no arm's smoke may rely on a
shell escape hatch.

These tests are hermetic: no opencode, no MCP server, no corpus, no provider.

    python3 -m unittest discover -s benchmarks/repo-qa/tests -t .
"""

import copy
import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

BENCH_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(BENCH_ROOT, "scripts")
sys.path.insert(0, SCRIPTS)
SPEC = importlib.util.spec_from_file_location("bench", os.path.join(SCRIPTS, "bench.py"))
bench = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bench)

TREATMENT_VARIANTS = ("lci", "lci-slim", "lci-ann")
BASELINE_VARIANTS = ("base",)

PREDICTIONS_PATH = os.path.join(BENCH_ROOT, "discovery", "predictions.json")


def _cfg(variant):
    return bench.workspace_config(variant, "/usr/bin/fake-lci")


def _denied(tools):
    return {k for k, v in tools.items() if v is False}


# Snapshots of the arms as registered in discovery/predictions.json
# (schema_version 2, registry_id d3-discovery-predictions-v1). The registry is
# a frozen pre-registration record; if these constants and the registry ever
# disagree, the registry file changed and this test MUST fail until the config
# gate below is consciously re-derived against the installed opencode version.
EXPECTED_TREATMENT_NATIVE = {"read"}
EXPECTED_BASELINE_NATIVE = {"glob", "grep", "read"}
EXPECTED_TREATMENT_DENIED = frozenset(bench.NATIVE_TOOL_IDS) - EXPECTED_TREATMENT_NATIVE
EXPECTED_BASELINE_DENIED = frozenset(bench.NATIVE_TOOL_IDS) - EXPECTED_BASELINE_NATIVE


class RegistryDerivationTest(unittest.TestCase):
    """Both arms' COMPLETE tool surface is derived from the registry lists."""

    def test_arms_loaded_from_predictions_json(self):
        import json
        with open(PREDICTIONS_PATH) as f:
            registry = json.load(f)
        self.assertEqual(set(bench.ARMS["treatment"]["tools"]),
                         set(registry["arms"]["treatment"]["tools"]))
        self.assertEqual(set(bench.ARMS["baseline"]["tools"]),
                         set(registry["arms"]["baseline"]["tools"]))

    def test_derivation_matches_registry_snapshot(self):
        self.assertEqual(set(bench.arm_tools("treatment")), set(EXPECTED_TREATMENT_DENIED))
        self.assertEqual(set(bench.arm_tools("baseline")), set(EXPECTED_BASELINE_DENIED))

    def test_treatment_variants_emit_the_registry_surface(self):
        for variant in TREATMENT_VARIANTS:
            with self.subTest(variant=variant):
                tools = _cfg(variant).get("tools", {})
                # Native keys must equal the derived arm surface exactly; the
                # lci_* auxiliary denials (SLIM_DISABLED) are layered on top by
                # workspace_config and are asserted by TreatmentIsolationTest.
                native = {k: v for k, v in tools.items() if not k.startswith("lci_")}
                self.assertEqual(native, bench.arm_tools("treatment"),
                                 f"{variant} native tools must equal the derived arm surface")

    def test_baseline_emits_the_registry_surface(self):
        tools = _cfg("base").get("tools", {})
        self.assertEqual(tools, bench.arm_tools("baseline"),
                         "base tools map must equal the derived arm surface")

    def test_treatment_denies_every_native_the_arm_does_not_list(self):
        for variant in TREATMENT_VARIANTS:
            with self.subTest(variant=variant):
                denied = _denied(_cfg(variant).get("tools", {}))
                for name in EXPECTED_TREATMENT_DENIED:
                    self.assertIn(name, denied,
                                  f"{variant} must deny native '{name}' (not in arms.treatment.tools)")
                self.assertNotIn("read", denied, "treatment arm lists Read")

    def test_baseline_denies_every_native_the_arm_does_not_list(self):
        denied = _denied(_cfg("base").get("tools", {}))
        for name in EXPECTED_BASELINE_DENIED:
            self.assertIn(name, denied,
                          f"base must deny native '{name}' (not in arms.baseline.tools)")
        self.assertNotIn("read", denied)
        self.assertNotIn("grep", denied)
        self.assertNotIn("glob", denied)

    def test_unknown_native_tool_in_registry_fails_fast(self):
        # If the registry names a native tool that is not in the verified
        # opencode surface, derivation must raise rather than silently grant a
        # tool nobody audited.
        original = bench.ARMS
        try:
            bench.ARMS = copy.deepcopy(original)
            bench.ARMS["baseline"]["tools"] = list(bench.ARMS["baseline"]["tools"]) + ["Telepathy"]
            with self.assertRaises(ValueError):
                bench.arm_tools("baseline")
        finally:
            bench.ARMS = original


class DriftTest(unittest.TestCase):
    """Drift between the registry lists and the emitted config fails both ways."""

    def test_registry_drift_breaks_the_snapshot_gate(self):
        # Direction 1: a registry that (re-)grants Grep to the treatment arm
        # changes the derived surface, which the frozen snapshot catches.
        original = bench.ARMS
        try:
            bench.ARMS = copy.deepcopy(original)
            bench.ARMS["treatment"]["tools"] = list(bench.ARMS["treatment"]["tools"]) + ["Grep"]
            drifted = bench.arm_tools("treatment")
            self.assertNotIn("grep", drifted,
                             "derivation follows the registry (no hardcoded deny)")
            self.assertNotEqual(set(drifted), set(EXPECTED_TREATMENT_DENIED),
                                "registry drift MUST fail the snapshot gate")
        finally:
            bench.ARMS = original

    def test_emitter_drift_breaks_the_surface_gate(self):
        # Direction 2: an emitter that forgets a denial diverges from the
        # derived surface the config test asserts equality against.
        good = _cfg("lci").get("tools", {})
        self.assertEqual(good, bench.arm_tools("treatment"))
        regressed = {k: v for k, v in good.items() if k != "grep"}
        self.assertNotEqual(regressed, bench.arm_tools("treatment"),
                            "a dropped denial MUST fail the derivation gate")


class TreatmentIsolationTest(unittest.TestCase):
    def test_every_lci_variant_disables_native_grep_and_glob(self):
        for variant in TREATMENT_VARIANTS:
            with self.subTest(variant=variant):
                tools = _cfg(variant).get("tools", {})
                self.assertIs(tools.get("grep"), False,
                              f"{variant} must disable native grep (disjoint arm)")
                self.assertIs(tools.get("glob"), False,
                              f"{variant} must disable native glob (disjoint arm)")

    def test_every_lci_variant_still_enables_lci_mcp(self):
        for variant in TREATMENT_VARIANTS:
            with self.subTest(variant=variant):
                cfg = _cfg(variant)
                self.assertTrue(cfg["mcp"]["lci"]["enabled"])

    def test_treatment_disables_bash_per_registry(self):
        # The registered treatment arm omits bash; bash reintroduces grep.
        for variant in TREATMENT_VARIANTS:
            with self.subTest(variant=variant):
                tools = _cfg(variant).get("tools", {})
                self.assertIs(tools.get("bash"), False,
                              f"{variant} must disable bash to keep the arm disjoint")

    def test_lci_auxiliaries_stay_enabled_except_in_slim(self):
        for variant in ("lci", "lci-ann"):
            with self.subTest(variant=variant):
                tools = _cfg(variant).get("tools", {})
                for name in bench.SLIM_DISABLED:
                    self.assertNotIn(f"lci_{name}", tools,
                                     f"{variant} must keep lci_{name} enabled")
        slim = _cfg("lci-slim").get("tools", {})
        for name in bench.SLIM_DISABLED:
            self.assertIs(slim.get(f"lci_{name}"), False,
                          f"lci-slim must disable auxiliary lci_{name}")


class BaselineDisjointTest(unittest.TestCase):
    def test_baseline_keeps_grep_and_glob_enabled(self):
        for variant in BASELINE_VARIANTS:
            with self.subTest(variant=variant):
                denied = _denied(_cfg(variant).get("tools", {}))
                self.assertNotIn("grep", denied, "baseline must keep native grep")
                self.assertNotIn("glob", denied, "baseline must keep native glob")
                self.assertNotIn("read", denied, "baseline must keep read")

    def test_baseline_denies_bash_per_registry(self):
        # Corrected bash decision: arms.baseline.tools lists ["Glob","Grep",
        # "Read"] only. The registry does NOT grant the baseline a shell.
        for variant in BASELINE_VARIANTS:
            with self.subTest(variant=variant):
                tools = _cfg(variant).get("tools", {})
                self.assertIs(tools.get("bash"), False,
                              f"{variant} must deny bash: the registry does not list it")

    def test_baseline_has_no_lci_mcp(self):
        for variant in BASELINE_VARIANTS:
            with self.subTest(variant=variant):
                cfg = _cfg(variant)
                self.assertNotIn("lci", cfg["mcp"])


class DiscriminationTest(unittest.TestCase):
    """A broken treatment config (no grep/glob disable) MUST be caught."""

    def test_handler_catches_a_treatment_that_forgets_to_disable_grep(self):
        good = _cfg("lci").get("tools", {})
        self.assertIs(good.get("grep"), False)
        # Simulate the pre-fix regression: the arm adds LCI but leaves grep on.
        regressed = {k: v for k, v in good.items() if k != "grep"}
        self.assertIsNot(regressed.get("grep"), False)
        self.assertFalse(_disjoint_ok(regressed))

    def test_handler_catches_a_baseline_that_over_disables(self):
        base_tools = _cfg("base").get("tools", {})
        self.assertTrue(_disjoint_ok(base_tools, arm="baseline"))
        over = dict(base_tools)
        over["grep"] = False
        self.assertFalse(_disjoint_ok(over, arm="baseline"))

    def test_handler_catches_a_baseline_that_grants_bash(self):
        # Simulate the pre-fix baseline: no tools map at all (bash stays live).
        self.assertFalse(_disjoint_ok({}, arm="baseline"))


class RunOneIsolationTest(unittest.TestCase):
    """`run_one` must launch through `opencode_runner.isolated_environment` with
    `OPENCODE_CONFIG` and `PWD` pinned INSIDE the corpus workspace and all four
    XDG base dirs pinned OUTSIDE it (an external per-cell state root), so a
    host-level opencode config can never re-allow a denied tool and opencode's
    own config/state/data/cache/log can never contaminate the measured copy.
    """

    XDG_VARS = ("XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME")

    def setUp(self):
        self._real_run = bench.subprocess.run
        self._real_isolated = bench.opencode_runner.isolated_environment
        self.captured = {}

        class _Proc:
            returncode = 0
            stdout = ""
            stderr = ""

        def fake_run(cmd, **kw):
            self.captured["cmd"] = cmd
            self.captured["cwd"] = kw.get("cwd")
            self.captured["env"] = kw.get("env")
            return _Proc()

        bench.subprocess.run = fake_run

    def tearDown(self):
        bench.subprocess.run = self._real_run
        bench.opencode_runner.isolated_environment = self._real_isolated

    def test_run_one_launches_through_isolated_environment(self):
        calls = []
        real = self._real_isolated

        def spy(workspace):
            calls.append(Path(workspace))
            return real(workspace)

        bench.opencode_runner.isolated_environment = spy
        with tempfile.TemporaryDirectory() as ws:
            bench.run_one(_CfgStub(), ws, "some/model", {"question": "x"}, 5)
        self.assertEqual(len(calls), 1,
                         "run_one must build its env via opencode_runner.isolated_environment")
        # The dir isolated_environment is pointed at holds the writable XDG
        # state, so it must NOT be the corpus workspace (that is the leak).
        self.assertFalse(calls[0].resolve().is_relative_to(Path(ws).resolve()),
                         "opencode's XDG state root must be external to the corpus workspace")

    def test_config_and_pwd_point_into_the_workspace(self):
        with tempfile.TemporaryDirectory() as ws:
            bench.run_one(_CfgStub(), ws, "some/model", {"question": "x"}, 5)
        env = self.captured["env"]
        self.assertIsNotNone(env, "run_one must pass an explicit env to opencode run")
        self.assertEqual(env.get("OPENCODE_CONFIG"), os.path.join(ws, "opencode.json"),
                         "the arm's opencode.json (inside the workspace) must be the config")
        self.assertEqual(env.get("PWD"), ws,
                         "PWD must be the workspace so opencode answers about the corpus copy")
        # The isolated config must not drop PATH (the opencode binary lookup).
        self.assertEqual(env.get("PATH"), os.environ.get("PATH"))

    def test_xdg_state_is_anchored_outside_the_workspace(self):
        with tempfile.TemporaryDirectory() as ws:
            bench.run_one(_CfgStub(), ws, "some/model", {"question": "x"}, 5)
        env = self.captured["env"]
        for var in self.XDG_VARS:
            self.assertIn(var, env, f"{var} must be pinned (never inherited from the host)")
            value = Path(env[var])
            self.assertTrue(value.is_absolute(), f"{var} must be an absolute path")
            self.assertFalse(value.resolve().is_relative_to(Path(ws).resolve()),
                             f"{var}={value} would write opencode state INSIDE the measured copy")

    def test_host_reanchor_variables_are_dropped(self):
        # OPENCODE_CONFIG_DIR re-anchors opencode's GLOBAL config dir and
        # bypasses XDG_CONFIG_HOME entirely; when bench is launched from inside
        # an opencode session it inherits a global `"*": "allow"` permission
        # that merges over the arm's tools map and silently re-grants native
        # grep to the treatment arm. The cell env must not carry it (verified
        # live on 1.18.31 by the results/ discrimination smoke).
        with mock.patch.dict(os.environ, {
                "OPENCODE_CONFIG_DIR": "/home/host/.config/opencode-session/opencode",
                "OPENCODE": "1",
                "OPENCODE_CLIENT": "acp"}):
            with tempfile.TemporaryDirectory() as ws:
                bench.run_one(_CfgStub(), ws, "some/model", {"question": "x"}, 5)
        env = self.captured["env"]
        for var in ("OPENCODE_CONFIG_DIR", "OPENCODE", "OPENCODE_CLIENT"):
            self.assertNotIn(var, env,
                             f"{var} must not reach the child: it can re-allow a tool the arm denies")


class WorkspacePollutionTest(unittest.TestCase):
    """Building a cell's launch env must create NOTHING inside the corpus
    workspace: opencode's config/state/data/cache/log dirs are anchored to an
    external state root. The pre-attempt-4 contract pointed isolated_environment
    straight at the workspace and littered it with `.xdg-*` (the base-arm grep in
    the prior smoke matched the generated opencode.log)."""

    def test_arm_environment_creates_no_opencode_path_in_the_workspace(self):
        with tempfile.TemporaryDirectory() as ws, tempfile.TemporaryDirectory() as state_root:
            with open(os.path.join(ws, "opencode.json"), "w") as f:
                f.write("{}\n")  # the arm config, written by ensure_workspace
            before = set(os.listdir(ws))
            env = bench.arm_environment(ws, state_root)
            after = set(os.listdir(ws))
            self.assertEqual(before, after,
                             "arm_environment must not create any path inside the workspace")
            for var in ("XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
                self.assertTrue(Path(env[var]).resolve().is_relative_to(Path(state_root).resolve()),
                                f"{var} must be under the external state root, not the workspace")

    def test_isolated_environment_would_pollute_the_workspace_directly(self):
        # Guard for WHY arm_environment cannot just call isolated_environment(ws):
        # pointing it at the workspace creates the `.xdg-*` dirs inside it.
        with tempfile.TemporaryDirectory() as ws:
            bench.opencode_runner.isolated_environment(Path(ws))
            leaked = [n for n in os.listdir(ws) if n.startswith(".xdg-")]
            self.assertTrue(leaked,
                            "isolated_environment(ws) is the leak source — arm_environment must avoid it")


class _CfgStub:
    def __init__(self):
        self.defaults = {"opencode-bin": "opencode"}


def _disjoint_ok(tools, arm="treatment"):
    if arm == "treatment":
        return tools.get("grep") is False and tools.get("glob") is False
    return tools.get("grep") is not False and tools.get("glob") is not False \
        and tools.get("bash") is False


if __name__ == "__main__":
    unittest.main()
