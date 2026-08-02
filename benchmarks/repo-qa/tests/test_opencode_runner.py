import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = ROOT / "benchmarks/repo-qa/scripts"
sys.path.insert(0, str(SCRIPTS))
SPEC = importlib.util.spec_from_file_location("opencode_runner", SCRIPTS / "opencode_runner.py")
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class ClassifyFailureTest(unittest.TestCase):
    def test_quota_markers_and_status_429_classify(self):
        self.assertEqual(runner.classify_failure("HTTP 429 rate limit exceeded"), "provider_quota")
        self.assertEqual(runner.classify_failure("provider returned status 429"), "provider_quota")
        self.assertIsNone(runner.classify_failure("ordinary provider failure"))

    def test_429_matches_only_on_a_token_boundary(self):
        self.assertIsNone(runner.classify_failure("session id 84290 failed"))
        self.assertIsNone(runner.classify_failure("elapsed 4429ms"))
        self.assertIsNone(runner.classify_failure("cost 0.429 credits"))


class IsolationTest(unittest.TestCase):
    def test_all_xdg_state_moves_into_the_workspace(self):
        with tempfile.TemporaryDirectory() as parent:
            workspace = runner.empty_git_workspace(Path(parent))
            environment = runner.isolated_environment(workspace)
            for variable in ("XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
                self.assertTrue(environment[variable].startswith(str(workspace)),
                                f"{variable} leaks outside the workspace")
            self.assertEqual(environment["OPENCODE_CONFIG"], str(workspace / "opencode.json"))


class ProcessGroupTest(unittest.TestCase):
    def test_timeout_kills_the_whole_process_group(self):
        marker = "300.424217"
        with tempfile.TemporaryDirectory() as parent:
            fake = Path(parent) / "fake-opencode"
            fake.write_text("#!/usr/bin/env python3\n"
                            "import subprocess, time\n"
                            f"subprocess.Popen(['sleep', '{marker}'])\n"
                            "time.sleep(300)\n")
            fake.chmod(0o755)
            workspace = runner.empty_git_workspace(Path(parent))
            result = runner.run_opencode(str(fake), workspace, "provider/model", "prompt", 1)
            self.assertEqual(result["status"], "provider_timeout")
            survivors = subprocess.run(["pgrep", "-f", f"sleep {marker}"],
                                       capture_output=True, text=True)
            self.assertEqual(survivors.stdout.strip(), "",
                             "grandchild survived the wrapper timeout")


if __name__ == "__main__":
    unittest.main()
