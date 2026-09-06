#!/usr/bin/env python3
"""Hermetic tests for the E2.3 selection harness.

No model is ever called here: every grading test runs against a RECORDED
opencode event stream, so the suite stays deterministic while the graded
quantity (which tool the model picked first) is exactly the one the live grid
measures.

Import note: the package under test lives at toolcalling/runner/, and this
repo ALREADY has an importable top-level package called `runner`
(benchmarks/repo-qa/exploration/runner, put on sys.path by
scripts/exploration_runner.py and its tests). Adding toolcalling/ to sys.path
would make `import runner` resolve to whichever test ran first. The package is
therefore loaded by path under the distinct name `selection_runner`.
"""

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

BENCH_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = BENCH_ROOT / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

MANIFEST = BENCH_ROOT / "comprehension/surface/tool-surface.json"
TASKS_DIR = BENCH_ROOT / "toolcalling/tasks"
VARIANT_A = BENCH_ROOT / "toolcalling/descriptions/variant-a-live-verbatim.json"


def load_selection_runner():
    root = BENCH_ROOT / "toolcalling" / "runner"
    if "selection_runner" in sys.modules:
        return sys.modules["selection_runner"]
    spec = importlib.util.spec_from_file_location(
        "selection_runner", root / "__init__.py",
        submodule_search_locations=[str(root)])
    module = importlib.util.module_from_spec(spec)
    sys.modules["selection_runner"] = module
    spec.loader.exec_module(module)
    return module


sr = load_selection_runner()
import selection_ab  # noqa: E402


def event(tool, args=None, status="completed", output="ok"):
    """Shape of a real `opencode run --format json` (1.18.26) tool event:
    top-level type `tool_use`, part type `tool`, args under state.input."""
    return json.dumps({"type": "tool_use", "part": {
        "type": "tool", "tool": tool,
        "state": {"status": status, "input": args or {}, "output": output},
    }})


def text_event(text="done"):
    return json.dumps({"type": "text", "part": {"type": "text",
                                                "messageID": "m1", "text": text}})


def task(task_id):
    return json.loads((TASKS_DIR / f"{task_id}.json").read_text())


CALLERS_TASK = "confirmed-call-sites-for-one-routine"


# --------------------------------------------------------------- arm disjointness

class WorkspaceDisjointnessTest(unittest.TestCase):
    """bench-harness-oracle-independence rule 12: the A/B arms must be PINNED
    disjoint by a test before any cell runs."""

    def test_config_offers_only_the_mock_mcp_server(self):
        cfg = sr.workspace.mock_workspace_config(VARIANT_A)
        enabled = [name for name, spec in cfg["mcp"].items()
                   if spec.get("enabled", True)]
        self.assertEqual(enabled, ["lci"])
        self.assertIn(str(sr.workspace.MOCK_SCRIPT), cfg["mcp"]["lci"]["command"])

    def test_variant_descriptions_are_the_only_thing_the_arm_changes(self):
        a = sr.workspace.mock_workspace_config(VARIANT_A)
        b = sr.workspace.mock_workspace_config(
            BENCH_ROOT / "toolcalling/descriptions/variant-b-task-framed.json")
        env_a = a["mcp"]["lci"].pop("environment")
        env_b = b["mcp"]["lci"].pop("environment")
        self.assertEqual(a, b)
        self.assertNotEqual(env_a["LCI_MOCK_DESCRIPTIONS"],
                            env_b["LCI_MOCK_DESCRIPTIONS"])

    def test_built_workspace_carries_no_corpus_for_native_tools_to_read(self):
        with tempfile.TemporaryDirectory() as parent:
            ws = sr.workspace.build_workspace(Path(parent), VARIANT_A)
            present = sorted(p.name for p in ws.iterdir() if p.name != ".git")
            self.assertEqual(present, [".gitignore", "opencode.json"])

    def test_the_mock_really_serves_the_variant_descriptions_over_stdio(self):
        """Disjointness is only real if the wired command answers tools/list."""
        cfg = sr.workspace.mock_workspace_config(VARIANT_A)
        spec = cfg["mcp"]["lci"]
        env = dict(os.environ, **spec["environment"])
        proc = subprocess.Popen(spec["command"], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, text=True, env=env)
        try:
            proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": 1,
                                         "method": "tools/list"}) + "\n")
            proc.stdin.flush()
            served = json.loads(proc.stdout.readline())["result"]["tools"]
        finally:
            proc.stdin.close()
            proc.stdout.close()
            proc.wait(timeout=10)
        expected = json.loads(VARIANT_A.read_text())["descriptions"]
        self.assertEqual({t["name"]: t["description"] for t in served}, expected)


# --------------------------------------------------------------- trace parsing

class TraceTest(unittest.TestCase):
    def test_calls_are_ordered_and_native_calls_are_kept(self):
        calls = sr.trace.parse_trace([
            text_event(), event("grep", {"pattern": "foo"}),
            event("lci_callers", {"symbol": "foo"}),
        ])
        self.assertEqual([(c["tool"], c["native"]) for c in calls],
                         [("grep", True), ("callers", False)])
        self.assertEqual([c["position"] for c in calls], [1, 2])
        self.assertEqual(calls[0]["args"], {"pattern": "foo"})

    def test_part_typed_tool_events_parse_too(self):
        line = json.dumps({"type": "part.updated", "part": {
            "type": "tool", "tool": "lci_search",
            "state": {"status": "completed", "input": {"query": "x"}}}})
        calls = sr.trace.parse_trace([line])
        self.assertEqual([c["tool"] for c in calls], ["search"])


# --------------------------------------------------------------------- grading

class GradingTest(unittest.TestCase):
    def setUp(self):
        self.grader = sr.grade.Grader.from_manifest(MANIFEST)
        self.task = task(CALLERS_TASK)

    def test_live_smoke_finding_is_recorded_as_a_mis_selection(self):
        """The weak model on the callers task under variant A opened with
        `search`. A harness that cannot detect the known failure is
        unverified."""
        record = self.grader.grade(self.task, sr.trace.parse_trace([
            event("lci_search", {"query": "processRequest"}),
        ]), run_status="answered")
        self.assertEqual(record["outcome"], "graded")
        self.assertFalse(record["selected_correct"])
        self.assertEqual(record["matrix_cell"], ("callers", "search"))
        self.assertFalse(record["correct_ever"])
        self.assertIsNone(record["first_correct_position"])

    def test_correct_tool_at_position_three_is_not_a_success(self):
        record = self.grader.grade(self.task, sr.trace.parse_trace([
            event("lci_search", {"query": "x"}),
            event("lci_get_context", {"symbol": "x"}),
            event("lci_callers", {"symbol": "x"}),
        ]), run_status="answered")
        self.assertFalse(record["selected_correct"])
        self.assertTrue(record["correct_ever"])
        self.assertEqual(record["first_correct_position"], 3)
        self.assertEqual(record["matrix_cell"], ("callers", "search"))

    def test_first_correct_call_is_a_success(self):
        record = self.grader.grade(self.task, sr.trace.parse_trace([
            event("lci_callers", {"symbol": "x"}),
        ]), run_status="answered")
        self.assertTrue(record["selected_correct"])
        self.assertEqual(record["first_correct_position"], 1)

    def test_a_native_call_before_any_mcp_call_is_the_first_call(self):
        record = self.grader.grade(self.task, sr.trace.parse_trace([
            event("grep", {"pattern": "x"}),
            event("lci_callers", {"symbol": "x"}),
        ]), run_status="answered")
        self.assertFalse(record["selected_correct"])
        self.assertTrue(record["first_call_native"])
        self.assertEqual(record["first_called_tool"], "native:grep")
        self.assertEqual(record["matrix_cell"], ("callers", "native:grep"))
        self.assertTrue(record["correct_ever"])
        self.assertEqual(record["first_correct_position"], 2)

    def test_quota_kill_is_named_and_excluded_from_the_matrix(self):
        record = self.grader.grade(self.task, [], run_status="provider_quota")
        self.assertEqual(record["outcome"], "provider_quota")
        self.assertFalse(record["counts_in_matrix"])
        self.assertIsNone(record["matrix_cell"])

    def test_timeout_and_dnf_are_distinct_named_outcomes(self):
        timed_out = self.grader.grade(self.task, [], run_status="provider_timeout")
        self.assertEqual(timed_out["outcome"], "provider_timeout")
        dnf = self.grader.grade(self.task, [], run_status="answered")
        self.assertEqual(dnf["outcome"], "no_tool_call")
        self.assertFalse(dnf["counts_in_matrix"])

    def test_a_hallucinated_tool_name_is_a_named_outcome(self):
        record = self.grader.grade(self.task, sr.trace.parse_trace([
            event("lci_find_callers", {"symbol": "x"}),
        ]), run_status="answered")
        self.assertEqual(record["outcome"], "hallucinated_tool")
        self.assertEqual(record["first_called_tool"], "hallucinated:find_callers")

    def test_no_record_is_ever_zeroed(self):
        for status in ("answered", "provider_quota", "provider_timeout",
                       "provider_error", "malformed_provider_stream", "exit_1"):
            record = self.grader.grade(self.task, [], run_status=status)
            self.assertTrue(record["outcome"])
            self.assertIn("counts_in_matrix", record)


class ArgsPlausibilityTest(unittest.TestCase):
    """Graded against the MANIFEST input_schema, not the mock's: the mock
    currently serves an empty permissive schema (follow-up
    01M1W40XF2E0HKT05SS51A5PJ2), so it would certify anything."""

    def setUp(self):
        self.grader = sr.grade.Grader.from_manifest(MANIFEST)
        self.task = task(CALLERS_TASK)

    def test_plausible_args_validate(self):
        record = self.grader.grade(self.task, sr.trace.parse_trace([
            event("lci_callers", {"name": "processRequest"}),
        ]), run_status="answered")
        self.assertTrue(record["args_plausible"])
        self.assertEqual(record["args_errors"], [])

    def test_type_wrong_args_are_implausible(self):
        record = self.grader.grade(self.task, sr.trace.parse_trace([
            event("lci_callers", {"name": ["processRequest"]}),
        ]), run_status="answered")
        self.assertFalse(record["args_plausible"])
        self.assertTrue(record["args_errors"])

    def test_grading_is_against_the_manifest_not_the_permissive_mock_schema(self):
        served = json.loads(json.dumps(sr.grade.Grader.from_manifest(MANIFEST)
                                       .schemas["callers"]))
        self.assertTrue(served.get("properties"))


# ---------------------------------------------------------------------- matrix

class MatrixTest(unittest.TestCase):
    def records(self):
        return [
            {"variant": "a", "model": "weak", "tier": "confusable",
             "correct_tool": "callers", "matrix_cell": ["callers", "search"],
             "counts_in_matrix": True, "outcome": "graded"},
            {"variant": "a", "model": "weak", "tier": "confusable",
             "correct_tool": "callers", "matrix_cell": ["callers", "callers"],
             "counts_in_matrix": True, "outcome": "graded"},
            {"variant": "a", "model": "weak", "tier": "control",
             "correct_tool": "index_stats",
             "matrix_cell": ["index_stats", "index_stats"],
             "counts_in_matrix": True, "outcome": "graded"},
            {"variant": "a", "model": "weak", "tier": "confusable",
             "correct_tool": "callers", "matrix_cell": None,
             "counts_in_matrix": False, "outcome": "provider_quota"},
        ]

    def test_matrix_is_per_variant_model_and_stratified_by_tier(self):
        cells = sr.matrix.confusion_matrices(self.records())
        keys = {(c["variant"], c["model"], c["tier"]) for c in cells}
        self.assertEqual(keys, {("a", "weak", "confusable"),
                                ("a", "weak", "control")})

    def test_quota_kill_is_out_of_the_denominator_but_reported(self):
        cells = {c["tier"]: c for c in sr.matrix.confusion_matrices(self.records())}
        confusable = cells["confusable"]
        self.assertEqual(confusable["denominator"], 2)
        self.assertEqual(confusable["excluded"], {"provider_quota": 1})
        self.assertEqual(confusable["rows"]["callers"],
                         {"search": 1, "callers": 1})

    def test_tiers_are_never_pooled(self):
        cells = sr.matrix.confusion_matrices(self.records())
        for cell in cells:
            self.assertNotIn("all", cell["tier"])
            self.assertEqual(sum(sum(r.values()) for r in cell["rows"].values()),
                             cell["denominator"])


# --------------------------------------------------------------- grid + resume

class GridTest(unittest.TestCase):
    def test_plan_is_the_full_task_by_variant_by_model_grid(self):
        cells = selection_ab.plan_cells(TASKS_DIR, ["a", "b"], ["weak", "strong"])
        n_tasks = len(list(TASKS_DIR.glob("*.json")))
        self.assertEqual(len(cells), n_tasks * 2 * 2)
        self.assertEqual(len({c["run_key"] for c in cells}), len(cells))

    def test_run_is_resume_safe_and_idempotent(self):
        calls = []

        def fake_cell(cell, **kwargs):
            calls.append(cell["run_key"])
            return {"run_key": cell["run_key"], "outcome": "graded",
                    "counts_in_matrix": True, "matrix_cell": ["callers", "search"],
                    "variant": cell["variant"], "model": cell["model"],
                    "tier": "confusable", "correct_tool": "callers"}

        with tempfile.TemporaryDirectory() as tmp:
            records = Path(tmp) / "records.jsonl"
            cells = selection_ab.plan_cells(TASKS_DIR, ["a"], ["weak"])[:3]
            first = selection_ab.run_cells(cells, records, fake_cell)
            self.assertEqual(len(first), 3)
            self.assertEqual(len(calls), 3)
            second = selection_ab.run_cells(cells, records, fake_cell)
            self.assertEqual(len(calls), 3, "resume must not re-run a done cell")
            self.assertEqual(len(second), 3)
            lines = records.read_text().strip().splitlines()
            self.assertEqual(len(lines), 3, "records must not duplicate")

    def test_provider_failures_are_retried_only_on_request(self):
        outcomes = iter(["provider_error", "graded"])

        def flaky_cell(cell, **kwargs):
            return {"run_key": cell["run_key"], "outcome": next(outcomes),
                    "counts_in_matrix": False, "matrix_cell": None,
                    "variant": cell["variant"], "model": cell["model"],
                    "tier": "confusable", "correct_tool": "callers"}

        with tempfile.TemporaryDirectory() as tmp:
            records = Path(tmp) / "records.jsonl"
            cells = selection_ab.plan_cells(TASKS_DIR, ["a"], ["weak"])[:1]
            selection_ab.run_cells(cells, records, flaky_cell)
            again = selection_ab.run_cells(cells, records, flaky_cell)
            self.assertEqual(again[0]["outcome"], "provider_error",
                             "default resume must not re-spend a call")
            retried = selection_ab.run_cells(cells, records, flaky_cell,
                                             retry_provider_failures=True)
            self.assertEqual(retried[0]["outcome"], "graded")
            self.assertEqual(selection_ab.read_records(records)[cells[0]["run_key"]]
                             ["outcome"], "graded", "last record wins on resume")
            # a graded cell is never retried, even with the flag
            selection_ab.run_cells(cells, records, flaky_cell,
                                   retry_provider_failures=True)

    def test_report_names_the_mock_schema_caveat(self):
        import io
        from contextlib import redirect_stdout
        with tempfile.TemporaryDirectory() as tmp:
            buf = io.StringIO()
            with redirect_stdout(buf):
                selection_ab.main(["report", "--records", str(Path(tmp) / "r.jsonl")])
        report = json.loads(buf.getvalue())
        self.assertTrue(any("01M1W40XF2E0HKT05SS51A5PJ2" in c and "mock" in c
                            for c in report["caveats"]))


if __name__ == "__main__":
    unittest.main()
