import importlib.util
import unittest
import tempfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/replay_common.py"
SPEC = importlib.util.spec_from_file_location("replay_common_writers", SCRIPT)
common = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(common)


class SharedWritersTest(unittest.TestCase):
    def test_atomic_write_replaces_and_creates_parents(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "nested" / "record.json"
            common.write_atomic(path, "one\n")
            common.write_atomic(path, "two\n")
            self.assertEqual(path.read_text(), "two\n")
            self.assertEqual([p.name for p in path.parent.iterdir()], ["record.json"])

    def test_atomic_write_failure_cleans_temp_and_keeps_original_error(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "record.json"
            path.write_text("original\n")
            boom = RuntimeError("replace failed")
            with mock.patch.object(common.os, "replace", side_effect=boom):
                with self.assertRaises(RuntimeError) as caught:
                    common.write_atomic(path, "new\n")
            self.assertIs(caught.exception, boom)
            self.assertEqual(path.read_text(), "original\n")
            self.assertEqual([p.name for p in Path(directory).iterdir()], ["record.json"])

    def test_immutable_write_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "cell.json"
            common.write_immutable(path, "frozen\n")
            with self.assertRaisesRegex(RuntimeError, "immutable"):
                common.write_immutable(path, "other\n")
            self.assertEqual(path.read_text(), "frozen\n")


if __name__ == "__main__":
    unittest.main()
