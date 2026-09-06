"""Gates for the E2.1 tool-selection bench setup.

Three artifacts are under test and each has an adversarial gate, not just a
happy path (see .claude/rules/bench-harness-oracle-independence.md):

  * toolcalling/tasks/       -- one goal-oriented selection task per live tool
                                plus a CONTROL task, and a confusable-neighbor
                                map that makes the confusion matrix meaningful.
  * scripts/mock_lci_mcp.py  -- a hermetic stdio MCP exposing the REAL tool
                                names with injectable descriptions and stub
                                handlers, so SELECTION is measured in isolation
                                from output comprehension and from real LCI.
  * the leak check           -- asserts no task prompt hands the agent a tool
                                name, with a discrimination test that injects
                                one and asserts the check FAILS.

The tool list is DERIVED from the committed surface manifest
(comprehension/surface/tool-surface.json, probed live by
scripts/enumerate_tool_surface.py), never hand-listed here: a hand list drifts
from the surface and the matrix silently stops being fully populated.
"""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
REPO_QA = ROOT / "benchmarks/repo-qa"
MANIFEST = REPO_QA / "comprehension/surface/tool-surface.json"
TASK_DIR = REPO_QA / "toolcalling/tasks"
MOCK_DIR = REPO_QA / "toolcalling/mock_mcp"
MOCK_SCRIPT = REPO_QA / "scripts/mock_lci_mcp.py"
BASELINE_DESCRIPTIONS = MOCK_DIR / "descriptions-baseline.json"

sys.path.insert(0, str(REPO_QA / "scripts"))


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(mod)
    return mod


mock_mcp = _load("mock_lci_mcp", MOCK_SCRIPT)


def live_tool_names() -> list[str]:
    return [t["name"] for t in json.loads(MANIFEST.read_text())["tools"]]


def load_tasks() -> list[dict]:
    return [json.loads(p.read_text()) for p in sorted(TASK_DIR.glob("*.json"))]


class TaskSetShapeTest(unittest.TestCase):
    def setUp(self):
        self.tools = live_tool_names()
        self.tasks = load_tasks()

    def test_tasks_are_wellformed(self):
        self.assertTrue(self.tasks)
        seen = set()
        for task in self.tasks:
            self.assertEqual(task["schema"], "toolcalling_selection_task_v1")
            self.assertEqual(task["version"], 1)
            self.assertNotIn(task["id"], seen)
            seen.add(task["id"])
            self.assertIn(task["correct_tool"], self.tools)
            self.assertTrue(task["prompt"].strip())
            self.assertTrue(task["rationale"].strip())
            self.assertIsInstance(task["control"], bool)
            self.assertIsInstance(task["confusable_neighbors"], list)
            for neighbor in task["confusable_neighbors"]:
                self.assertIn(neighbor, self.tools)
                self.assertNotEqual(neighbor, task["correct_tool"])

    def test_every_task_cites_live_description_evidence_for_its_answer(self):
        """The answer key must be keyed to what the tool ACTUALLY does.

        Review attempt 1 found three tasks whose "correct" tool could not
        serve the stated need, because the baseline descriptions had been
        rewritten by hand and the tasks were keyed to the invention. Each task
        therefore quotes a VERBATIM fragment of the live manifest description
        of its correct tool; a fabricated capability has no such fragment.
        """
        live = {t["name"]: t["description"]
                for t in json.loads(MANIFEST.read_text())["tools"]}
        for task in self.tasks:
            evidence = task["answer_key_evidence"]
            self.assertTrue(evidence.strip(), task["id"])
            self.assertIn(
                evidence, live[task["correct_tool"]],
                f"{task['id']}: answer-key evidence is not in the live description "
                f"of its correct tool")

    def test_evidence_gate_rejects_an_invented_capability(self):
        live = {t["name"]: t["description"]
                for t in json.loads(MANIFEST.read_text())["tools"]}
        invented = "returns the lines surrounding a file position"
        self.assertNotIn(invented, live["context"])

    def test_every_live_tool_has_a_correct_selection_task(self):
        self.assertEqual(
            sorted({t["correct_tool"] for t in self.tasks}), sorted(self.tools))

    def test_every_live_tool_is_a_confusable_neighbor_somewhere(self):
        self.assertEqual(
            sorted(mock_mcp.neighbor_coverage(self.tasks)), sorted(self.tools))

    def test_matrix_population_fails_when_a_tool_loses_its_task(self):
        """Removing one tool's task must be caught, not silently tolerated."""
        dropped = "list_symbols"
        pruned = [t for t in self.tasks if t["correct_tool"] != dropped]
        covered = {t["correct_tool"] for t in pruned}
        self.assertNotEqual(sorted(covered), sorted(self.tools))
        self.assertNotIn(dropped, covered)

    def test_matrix_population_fails_when_a_tool_loses_neighbor_status(self):
        stripped = [dict(t, confusable_neighbors=[]) for t in self.tasks]
        self.assertEqual(mock_mcp.neighbor_coverage(stripped), set())

    def test_a_control_task_exists_and_is_unambiguous(self):
        controls = [t for t in self.tasks if t["control"]]
        self.assertTrue(controls)
        for task in controls:
            self.assertEqual(task["confusable_neighbors"], [])


class LeakCheckTest(unittest.TestCase):
    """A prompt naming its tool hands the agent the answer; the measure is void."""

    def setUp(self):
        self.tools = live_tool_names()
        self.tasks = load_tasks()

    def test_no_task_text_leaks_a_tool_name(self):
        self.assertEqual(mock_mcp.find_tool_name_leaks(self.tasks, self.tools), [])

    def test_leak_check_catches_a_full_tool_name(self):
        planted = [dict(self.tasks[0], prompt="Call list_symbols on that file.")]
        self.assertTrue(mock_mcp.find_tool_name_leaks(planted, self.tools))

    def test_leak_check_catches_reformatted_granularities(self):
        for form in ("list symbols", "listSymbols", "listsymbols",
                     "List-Symbols", "get_context", "GetContext"):
            planted = [dict(self.tasks[0], prompt=f"Use {form} here.")]
            self.assertTrue(
                mock_mcp.find_tool_name_leaks(planted, self.tools),
                f"leak check missed granularity {form!r}")

    def test_leak_check_catches_a_bare_discriminating_verb(self):
        """Single-word names and tool-identifying verbs are giveaways too."""
        for word in ("search", "browse", "inspect"):
            planted = [dict(self.tasks[0], prompt=f"Please {word} the corpus.")]
            self.assertTrue(
                mock_mcp.find_tool_name_leaks(planted, self.tools),
                f"leak check missed bare verb {word!r}")

    def test_leak_check_does_not_flag_generic_domain_words(self):
        clean = [dict(self.tasks[0],
                      prompt="Which file holds the symbol I need to change?")]
        self.assertEqual(mock_mcp.find_tool_name_leaks(clean, self.tools), [])

    def test_leak_report_is_redacted(self):
        planted = [dict(self.tasks[0], prompt="Call list_symbols now.")]
        report = " ".join(str(x) for x in
                          mock_mcp.find_tool_name_leaks(planted, self.tools))
        self.assertNotIn("list_symbols", report)


class MockMcpHandshakeTest(unittest.TestCase):
    """Reuses tooleval.McpSession -- the same stdio client the real bench uses."""

    def setUp(self):
        import tooleval
        self.tooleval = tooleval
        self.tools = live_tool_names()

    def _session(self, descriptions_path: Path | None = None):
        env = dict(os.environ)
        if descriptions_path is not None:
            env["LCI_MOCK_DESCRIPTIONS"] = str(descriptions_path)
        # McpSession spawns [bin, "mcp"]; a wrapper keeps that contract.
        proc_env = env
        session = self.tooleval.McpSession.__new__(self.tooleval.McpSession)
        session.proc = subprocess.Popen(
            [sys.executable, str(MOCK_SCRIPT), "mcp"], cwd=str(REPO_QA),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, env=proc_env)
        session.next_id = 1
        session._rpc("initialize", {
            "protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "toolcalling-test", "version": "0"}})
        session._notify("notifications/initialized")
        self.addCleanup(self._close, session)
        return session

    @staticmethod
    def _close(session):
        if session.proc.poll() is None:
            session.close()
        session.proc.stdout.close()

    def test_tools_list_exposes_every_live_tool_name(self):
        listed = self._session()._rpc("tools/list", {})["result"]["tools"]
        self.assertEqual(sorted(t["name"] for t in listed), sorted(self.tools))

    def test_tools_list_serves_the_injected_descriptions(self):
        variant = {name: f"VARIANT-{name}" for name in self.tools}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "variant.json"
            path.write_text(json.dumps(
                {"schema": "toolcalling_descriptions_v1", "version": 1,
                 "variant": "test", "descriptions": variant}))
            listed = self._session(path)._rpc("tools/list", {})["result"]["tools"]
        self.assertEqual({t["name"]: t["description"] for t in listed}, variant)

    def test_baseline_descriptions_cover_exactly_the_live_surface(self):
        payload = json.loads(BASELINE_DESCRIPTIONS.read_text())
        self.assertEqual(sorted(payload["descriptions"]), sorted(self.tools))
        for text in payload["descriptions"].values():
            self.assertTrue(text.strip())

    def test_baseline_descriptions_are_the_live_descriptions_verbatim(self):
        """The baseline arm must measure the REAL surface, not a paraphrase.

        A hand-written baseline is already a wording variant, so the E2.2
        deltas would be measured against an arm nobody ships, and any invented
        capability in it silently re-keys the answer set.
        """
        live = {t["name"]: t["description"]
                for t in json.loads(MANIFEST.read_text())["tools"]}
        payload = json.loads(BASELINE_DESCRIPTIONS.read_text())
        self.assertEqual(payload["descriptions"], live)

    def test_verbatim_gate_catches_a_reworded_baseline(self):
        live = {t["name"]: t["description"]
                for t in json.loads(MANIFEST.read_text())["tools"]}
        reworded = dict(live, search="Find text in the corpus.")
        self.assertNotEqual(reworded, live)

    def test_every_handler_returns_a_deterministic_stub_recording_the_call(self):
        session = self._session()
        first = {name: session.call_tool(name, {"q": "x"})[0]["text"]
                 for name in self.tools}
        second = {name: session.call_tool(name, {"q": "x"})[0]["text"]
                  for name in self.tools}
        self.assertEqual(first, second)
        for name, text in first.items():
            self.assertIn(name, text)
            self.assertIn('"q": "x"', text)
            self.assertTrue(text.startswith("OK: called "))

    def test_mock_never_shells_out_to_a_real_lci_binary(self):
        source = MOCK_SCRIPT.read_text()
        for forbidden in ("subprocess", "build/release/src/lci"):
            self.assertNotIn(forbidden, source)


if __name__ == "__main__":
    unittest.main()
