"""Contract tests for the Stage-1 exploration runner.

Every test is hermetic and network-free: a tiny forged tree + manifest is
synthesised in a tempdir (reusing the forge's emit shape), and a deterministic
fake agent yields canned tool-call sequences. No real corpora, no credentials.

The bullets these tests pin (one acceptance criterion each, at least):
  * config parity: both arms share model / system prompt / timeout / checkout,
    and differ ONLY in the allowed toolset + arm-specific tool instructions;
  * tool isolation is ENFORCED (a baseline run that calls LCI, shell-escapes,
    edits, or reads author-only data is rejected), not merely declared;
  * the per-run record captures the mandated fields and the effective allowlist;
  * tree-hash rejection fails loud when the tree drifts from its manifest;
  * resume is idempotent and distinguishes retryable failures from answers;
  * append writes survive a torn/interrupted tail line.
"""

import json
import os
import sys
import unittest
from tempfile import TemporaryDirectory

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH_ROOT = os.path.dirname(HERE)
EXPLORATION_ROOT = os.path.join(BENCH_ROOT, "exploration")
SCRIPTS = os.path.join(BENCH_ROOT, "scripts")
for _p in (EXPLORATION_ROOT, SCRIPTS):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import exploration_corpus_forge as forge  # noqa: E402
from runner import corpus, gate, record, run, toolsets  # noqa: E402
from runner.adapter import AgentResult, FakeAgent, ToolCall  # noqa: E402

FAKE_COMMIT = "0" * 40


def forge_fixture(root, corpus_id="pocketbase", seed=7):
    """Synthesise a forged corpus dir (manifest.json + tree/) the runner reads.

    Mirrors the forge's on-disk emit shape: <root>/<corpus_id>/seed-<seed>/
    with a tree/ holding real files and a manifest.json pinning the tree hash.
    """
    corpus_dir = os.path.join(root, corpus_id, f"seed-{seed}")
    tree = os.path.join(corpus_dir, "tree")
    os.makedirs(os.path.join(tree, "apis"))
    with open(os.path.join(tree, "apis", "base.go"), "w") as handle:
        handle.write("package apis\n\nfunc NewRouter() {}\n")
    with open(os.path.join(tree, "apis", "login.go"), "w") as handle:
        handle.write("package apis\n\nfunc recordAuthWithPassword() {}\n")
    path_map = {rel: rel for rel in forge.list_files(tree)}
    manifest = {
        "schema": forge.MANIFEST_SCHEMA,
        "corpus_id": corpus_id,
        "source_path": "/nonexistent/source",
        "source_commit": FAKE_COMMIT,
        "seed": seed,
        "forge_version": forge.FORGE_VERSION,
        "overrides": [],
        "mutations": [],
        "path_map": path_map,
        "decoys": [],
        "tree_hash": forge.tree_hash(tree),
        "validation": {"passed": True},
        "status": "ready",
    }
    forge._write_json_atomic(os.path.join(corpus_dir, "manifest.json"), manifest)
    return corpus_dir, tree, manifest


def fake_task(corpus_id="pocketbase", seed=7, task_id="pb-password-login-route"):
    return {
        "schema": "exploration_task_v1",
        "id": task_id,
        "corpus": corpus_id,
        "manifest_ref": {
            "corpus_id": corpus_id,
            "source_commit": FAKE_COMMIT,
            "seed": seed,
            "forge_version": forge.FORGE_VERSION,
        },
        "prompt": "Where is the identity/secret handler and where is the router built?",
        "rubric": {"must_surface": ["handler", "router"], "answer_shape": "two funcs"},
        "evidence": [],
        "adjudication": {"annotators": ["ann-hyde", "ann-quill"], "resolved": True},
    }


def base_config():
    return run.BaseConfig(
        model="claude-test-model",
        system_prompt="You explore code. Answer the question.",
        timeout_seconds=120,
    )


def answered_result(tool_calls):
    return AgentResult(
        status_hint="ok",
        final_answer="handler=recordAuthWithPassword router=NewRouter",
        tool_calls=tuple(tool_calls),
        input_tokens=1000,
        output_tokens=200,
        transcript={"turns": [{"role": "assistant", "text": "done"}]},
    )


class ConfigParityTest(unittest.TestCase):
    def test_arms_differ_only_in_toolset_and_instructions(self):
        base = base_config()
        checkout = "/tmp/checkout-x"
        prompt = "find the thing"
        treatment = toolsets.build_request(base, toolsets.TREATMENT, checkout, prompt)
        baseline = toolsets.build_request(base, toolsets.BASELINE, checkout, prompt)

        # identical across arms
        for field in ("model", "system_prompt", "timeout_seconds", "checkout_dir", "prompt"):
            self.assertEqual(
                getattr(treatment, field), getattr(baseline, field), field
            )

        # differ ONLY here
        self.assertNotEqual(treatment.allowed_tools, baseline.allowed_tools)
        self.assertNotEqual(treatment.tool_instructions, baseline.tool_instructions)

        # baseline exposes only lexical/discovery/read; treatment exposes LCI
        self.assertIn("Grep", baseline.allowed_tools)
        self.assertIn("Glob", baseline.allowed_tools)
        self.assertIn("Read", baseline.allowed_tools)
        self.assertFalse(any("lci" in t for t in baseline.allowed_tools))
        self.assertTrue(any("lci" in t for t in treatment.allowed_tools))
        # neither arm may edit or shell-escape
        for arm in (treatment.allowed_tools, baseline.allowed_tools):
            self.assertNotIn("Edit", arm)
            self.assertNotIn("Write", arm)
            self.assertNotIn("Bash", arm)


class ToolIsolationTest(unittest.TestCase):
    def test_baseline_calling_lci_is_rejected(self):
        with TemporaryDirectory() as root:
            forge_fixture(root)
            task = fake_task()
            records_path = os.path.join(root, "records.jsonl")
            work = os.path.join(root, "work")
            # baseline agent tries to reach for an LCI tool it must not have
            adapter = FakeAgent(
                answered_result([ToolCall("mcp__lci__search", {"query": "auth"})])
            )
            rec = run.run_task(
                task, toolsets.BASELINE, adapter, base_config(),
                corpus_root=root, records_path=records_path, work_root=work,
            )
            self.assertEqual(rec["status"], record.STATUS_TOOL_VIOLATION)
            self.assertTrue(rec["violations"])
            self.assertEqual(rec["violations"][0]["reason"], "tool_not_allowed")
            # effective allowlist is recorded, and it excludes LCI
            self.assertIn("effective_allowlist", rec)
            self.assertFalse(any("lci" in t for t in rec["effective_allowlist"]))

    def test_reading_outside_checkout_is_rejected(self):
        with TemporaryDirectory() as root:
            forge_fixture(root)
            task = fake_task()
            records_path = os.path.join(root, "records.jsonl")
            work = os.path.join(root, "work")
            # try to read author-only annotation data via an absolute escape
            leak = os.path.join(EXPLORATION_ROOT, "annotations")
            adapter = FakeAgent(
                answered_result([ToolCall("Read", {"file_path": leak})])
            )
            rec = run.run_task(
                task, toolsets.BASELINE, adapter, base_config(),
                corpus_root=root, records_path=records_path, work_root=work,
            )
            self.assertEqual(rec["status"], record.STATUS_TOOL_VIOLATION)
            self.assertEqual(rec["violations"][0]["reason"], "path_escape")


class RecordShapeTest(unittest.TestCase):
    def test_answered_run_records_every_mandated_field(self):
        with TemporaryDirectory() as root:
            _, _, manifest = forge_fixture(root)
            task = fake_task()
            records_path = os.path.join(root, "records.jsonl")
            work = os.path.join(root, "work")
            calls = [
                ToolCall("Grep", {"pattern": "func"}),
                ToolCall("Read", {"file_path": "apis/base.go"}),
            ]
            adapter = FakeAgent(answered_result(calls))
            rec = run.run_task(
                task, toolsets.BASELINE, adapter, base_config(),
                corpus_root=root, records_path=records_path, work_root=work,
            )
            self.assertEqual(rec["status"], record.STATUS_ANSWERED)
            self.assertEqual(rec["task_id"], task["id"])
            self.assertEqual(rec["corpus_id"], "pocketbase")
            self.assertEqual(rec["manifest_id"], manifest["tree_hash"])
            self.assertEqual(rec["arm"], toolsets.BASELINE)
            self.assertEqual(rec["model"], "claude-test-model")
            self.assertEqual(rec["seed"], 7)
            for field in ("started_at", "ended_at"):
                self.assertTrue(rec[field])
            self.assertGreaterEqual(rec["duration_seconds"], 0.0)
            self.assertIn("recordAuthWithPassword", rec["final_answer"])
            # ordered tool calls preserved
            self.assertEqual([c["name"] for c in rec["tool_calls"]], ["Grep", "Read"])
            # token usage extracted
            self.assertEqual(rec["token_usage"], {"input": 1000, "output": 200})
            # raw transcript reference points at an on-disk transcript
            self.assertTrue(os.path.isfile(rec["transcript_ref"]))
            with open(rec["transcript_ref"]) as handle:
                self.assertEqual(
                    json.load(handle),
                    {"turns": [{"role": "assistant", "text": "done"}]},
                )

    def test_provider_timeout_status(self):
        with TemporaryDirectory() as root:
            forge_fixture(root)
            task = fake_task()
            records_path = os.path.join(root, "records.jsonl")
            work = os.path.join(root, "work")
            adapter = FakeAgent(
                AgentResult("timeout", None, (), 0, 0, {"error": "deadline"})
            )
            rec = run.run_task(
                task, toolsets.BASELINE, adapter, base_config(),
                corpus_root=root, records_path=records_path, work_root=work,
            )
            self.assertEqual(rec["status"], record.STATUS_TIMEOUT)


class TreeHashRejectionTest(unittest.TestCase):
    def test_prepare_checkout_rejects_drifted_tree(self):
        with TemporaryDirectory() as root:
            _, tree, _ = forge_fixture(root)
            # drift: append a byte to a tracked file after the manifest was pinned
            with open(os.path.join(tree, "apis", "base.go"), "a") as handle:
                handle.write("// tamper\n")
            with self.assertRaises(corpus.TreeHashMismatch):
                corpus.prepare_checkout(
                    root, fake_task()["manifest_ref"], os.path.join(root, "co")
                )

    def test_run_task_records_config_error_on_drift(self):
        with TemporaryDirectory() as root:
            _, tree, _ = forge_fixture(root)
            with open(os.path.join(tree, "apis", "base.go"), "a") as handle:
                handle.write("// tamper\n")
            adapter = FakeAgent(answered_result([]))
            rec = run.run_task(
                fake_task(), toolsets.BASELINE, adapter, base_config(),
                corpus_root=root,
                records_path=os.path.join(root, "records.jsonl"),
                work_root=os.path.join(root, "work"),
            )
            self.assertEqual(rec["status"], record.STATUS_CONFIG_ERROR)
            self.assertEqual(adapter.calls, [])  # agent never invoked on a bad tree

    def test_missing_corpus_is_config_error(self):
        with TemporaryDirectory() as root:
            # no forge_fixture -> corpus absent
            adapter = FakeAgent(answered_result([]))
            rec = run.run_task(
                fake_task(), toolsets.BASELINE, adapter, base_config(),
                corpus_root=root,
                records_path=os.path.join(root, "records.jsonl"),
                work_root=os.path.join(root, "work"),
            )
            self.assertEqual(rec["status"], record.STATUS_CONFIG_ERROR)
            self.assertEqual(adapter.calls, [])


class ResumeTest(unittest.TestCase):
    def test_answered_record_is_not_rerun(self):
        with TemporaryDirectory() as root:
            forge_fixture(root)
            task = fake_task()
            records_path = os.path.join(root, "records.jsonl")
            work = os.path.join(root, "work")
            first = FakeAgent(answered_result([ToolCall("Read", {"file_path": "apis/base.go"})]))
            run.run_task(task, toolsets.BASELINE, first, base_config(),
                         corpus_root=root, records_path=records_path, work_root=work)
            # second attempt must be skipped without touching the adapter
            second = FakeAgent(answered_result([]))
            rec = run.run_task(task, toolsets.BASELINE, second, base_config(),
                               corpus_root=root, records_path=records_path, work_root=work)
            self.assertTrue(rec["skipped"])
            self.assertEqual(second.calls, [])
            # exactly one record persisted for this key
            keys = [r["run_key"] for r in record.load_records(records_path)]
            self.assertEqual(keys.count(record.run_key(task["id"], toolsets.BASELINE, 7)), 1)

    def test_retryable_failure_is_rerun(self):
        with TemporaryDirectory() as root:
            forge_fixture(root)
            task = fake_task()
            records_path = os.path.join(root, "records.jsonl")
            work = os.path.join(root, "work")
            failed = FakeAgent(AgentResult("provider_error", None, (), 0, 0, {"e": 1}))
            run.run_task(task, toolsets.BASELINE, failed, base_config(),
                         corpus_root=root, records_path=records_path, work_root=work)
            retry = FakeAgent(answered_result([ToolCall("Read", {"file_path": "apis/base.go"})]))
            rec = run.run_task(task, toolsets.BASELINE, retry, base_config(),
                               corpus_root=root, records_path=records_path, work_root=work)
            self.assertFalse(rec.get("skipped"))
            self.assertEqual(rec["status"], record.STATUS_ANSWERED)
            self.assertEqual(len(retry.calls), 1)


class AppendSafetyTest(unittest.TestCase):
    def test_torn_tail_line_is_tolerated(self):
        with TemporaryDirectory() as root:
            records_path = os.path.join(root, "records.jsonl")
            record.append_record(records_path, {"run_key": "a", "status": "answered"})
            record.append_record(records_path, {"run_key": "b", "status": "answered"})
            # simulate a crash mid-write: a partial, newline-less trailing line
            with open(records_path, "a") as handle:
                handle.write('{"run_key": "c", "sta')
            loaded = record.load_records(records_path)
            self.assertEqual([r["run_key"] for r in loaded], ["a", "b"])
            self.assertEqual(
                record.completed_keys(records_path), {"a", "b"}
            )

    def test_corrupt_interior_line_fails_loud(self):
        with TemporaryDirectory() as root:
            records_path = os.path.join(root, "records.jsonl")
            with open(records_path, "w") as handle:
                handle.write("not json\n")
                handle.write('{"run_key": "b", "status": "answered"}\n')
            with self.assertRaises(record.CorruptRecords):
                record.load_records(records_path)


if __name__ == "__main__":
    unittest.main()
