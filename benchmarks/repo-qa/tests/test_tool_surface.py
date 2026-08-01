import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/enumerate_tool_surface.py"
MANIFEST = ROOT / "benchmarks/repo-qa/comprehension/surface/tool-surface.json"
SPEC = importlib.util.spec_from_file_location("enumerate_tool_surface", SCRIPT)
surface = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(surface)


class ToolSurfaceTest(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads(MANIFEST.read_text())

    def test_manifest_captures_every_curated_live_tool(self):
        tools = self.manifest["tools"]
        self.assertEqual(self.manifest["tool_count"], len(tools))
        self.assertEqual([t["name"] for t in tools], sorted(surface.CASES))
        for tool in tools:
            self.assertIsInstance(tool["input_schema"], dict)
            self.assertIn("output_schema", tool)
            self.assertTrue(tool["question"])
            self.assertTrue(tool["oracle_command"])
            self.assertIsInstance(tool["answer"], list)
            self.assertIn("oracle_output", tool)
            for answer in tool["answer"]:
                self.assertIn(answer, tool["oracle_output"])

    def test_direct_conformance_bank_covers_the_live_surface(self):
        cases_path = ROOT / "benchmarks/repo-qa/tool-cases/chi.json"
        cases = json.loads(cases_path.read_text())["cases"]
        covered = {case["tool"] for case in cases}
        live = {tool["name"] for tool in self.manifest["tools"]}
        self.assertEqual(covered, live)

    def test_manifest_is_canonical_and_byte_stable(self):
        self.assertEqual(MANIFEST.read_text(), surface.canonical_json(self.manifest))

    def test_controls_are_retained(self):
        controls = {t["name"] for t in self.manifest["tools"] if t.get("control")}
        self.assertEqual(controls, {"index_stats", "info", "semantic_annotations"})

    def test_independent_oracle_discriminates_a_wrong_answer(self):
        case = next(t for t in self.manifest["tools"] if t["name"] == "search")
        deliberately_wrong = ["examples/base/main.go:610", "pocketbase.go:653"]
        for wrong in deliberately_wrong:
            self.assertNotIn(wrong, case["oracle_output"])
        self.assertEqual(
            set(case["answer"]),
            {"examples/base/main.go:119", "pocketbase.go:166"},
        )

    def test_oracle_execution_fails_closed(self):
        with self.assertRaises(RuntimeError):
            surface.run_oracle("printf partial; exit 7", ROOT)

    def test_no_match_can_be_explicit_without_masking_tool_failure(self):
        self.assertEqual(surface.run_oracle("exit 1", ROOT, {0, 1}), "")
        with self.assertRaises(RuntimeError):
            surface.run_oracle("exit 127", ROOT, {0, 1})


class McpSessionLivenessTest(unittest.TestCase):
    HANG = "#!/usr/bin/env python3\nimport sys, time\nsys.stdin.readline()\ntime.sleep(600)\n"

    def _hanging_binary(self, directory):
        path = Path(directory) / "hanging-lci"
        path.write_text(self.HANG)
        path.chmod(0o755)
        return path

    def test_rpc_fails_fast_and_kills_a_silent_child(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = self._hanging_binary(directory)
            with self.assertRaisesRegex(RuntimeError, "did not respond"):
                surface.McpSession(binary, Path(directory), read_timeout=1.0)

    def test_close_kills_a_child_that_ignores_stdin_close(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = self._hanging_binary(directory)
            session = surface.McpSession.__new__(surface.McpSession)
            session.proc = surface.subprocess.Popen(
                [str(binary)], cwd=directory, text=True,
                stdin=surface.subprocess.PIPE, stdout=surface.subprocess.PIPE,
                stderr=surface.subprocess.DEVNULL,
            )
            session.next_id = 1
            session.read_timeout = 1.0
            session.close(wait_timeout=1.0)
            self.assertIsNotNone(session.proc.poll())


if __name__ == "__main__":
    unittest.main()
