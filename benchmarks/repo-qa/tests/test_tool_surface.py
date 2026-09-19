import importlib.util
import json
import tempfile
import unittest
import unittest.mock
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/enumerate_tool_surface.py"
MANIFEST = ROOT / "benchmarks/repo-qa/comprehension/surface/tool-surface.json"
LCI_BIN = ROOT / "build/release/src/lci"
LIVE_CORPUS = ROOT / "benchmarks/repo-qa/.work/pocketbase-base"
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

    def test_manifest_equals_the_live_tools_list(self):
        """A committed snapshot is only as fresh as its last probe.

        Nothing else in the suite compares the manifest to the binary, so a
        tool added to the server (callers, 01ba88f) silently left every bench
        built on this file measuring a tool set that no longer exists. This
        gate SKIPS with a named reason when the binary or corpus is absent --
        it must never pass vacuously by treating absence as agreement.
        """
        if not LCI_BIN.exists():
            self.skipTest(
                f"live surface unverified: no LCI binary at {LCI_BIN}; "
                "build it (cmake --build build/release --target lci) to run this gate")
        if not LIVE_CORPUS.is_dir():
            self.skipTest(
                f"live surface unverified: no corpus at {LIVE_CORPUS}; "
                "the server needs a directory to serve tools/list from")
        session = surface.McpSession(LCI_BIN, LIVE_CORPUS)
        try:
            live = session.rpc("tools/list")["tools"]
        finally:
            session.close()
        self.assertEqual(
            {t["name"]: t.get("description", "") for t in live},
            {t["name"]: t["description"] for t in self.manifest["tools"]},
            "committed tool-surface manifest no longer matches the live tools/list; "
            "re-run scripts/enumerate_tool_surface.py")
        self.assertEqual(
            {t["name"]: t.get("inputSchema") for t in live},
            {t["name"]: t["input_schema"] for t in self.manifest["tools"]},
            "committed tool-surface input schemas no longer match the live "
            "tools/list; re-run scripts/enumerate_tool_surface.py")

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

    def test_oracle_shell_is_not_a_login_shell(self):
        """A login profile could rewrite PATH/aliases and skew oracle output."""
        recorded = {}
        real_run = surface.subprocess.run
        def spy(argv, **kwargs):
            recorded["argv"] = argv
            return real_run(argv, **kwargs)
        with unittest.mock.patch.object(surface.subprocess, "run", side_effect=spy):
            surface.run_oracle("true", ROOT)
        self.assertEqual(recorded["argv"][:2], ["bash", "-c"])


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

    def test_rpc_kills_the_child_before_raising_on_garbage_stdout(self):
        with tempfile.TemporaryDirectory() as directory:
            garbage = Path(directory) / "garbage-lci"
            garbage.write_text("#!/usr/bin/env python3\nimport sys, time\n"
                               "sys.stdin.readline()\nprint('not json', flush=True)\n"
                               "time.sleep(600)\n")
            garbage.chmod(0o755)
            with self.assertRaisesRegex(RuntimeError, "non-JSON"):
                surface.McpSession(garbage, Path(directory), read_timeout=5.0)

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
