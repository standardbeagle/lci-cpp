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
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH_ROOT = os.path.dirname(HERE)
EXPLORATION_ROOT = os.path.join(BENCH_ROOT, "exploration")
SCRIPTS = os.path.join(BENCH_ROOT, "scripts")
for _p in (EXPLORATION_ROOT, SCRIPTS):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import exploration_corpus_forge as forge  # noqa: E402
from runner import corpus, gate, record, run, toolsets  # noqa: E402
from runner.adapter import (  # noqa: E402
    AgentRequest,
    AgentResult,
    ClaudeCliAdapter,
    FakeAgent,
    ToolCall,
)
from task_digest import task_digest  # noqa: E402

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
        "claim": "The request's described behavior exists in this checkout.",
        "request": "Determine whether the described identity and routing behavior holds.",
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


def claim_result(tool_calls=(), verdict="false"):
    return AgentResult(
        status_hint="ok",
        final_answer=json.dumps({
            "verdict": verdict,
            "evidence": [{"path": "apis/base.go", "line": 3}],
            "rationale": "The router, rather than the helper alone, owns wiring.",
        }),
        tool_calls=tuple(tool_calls), input_tokens=11, output_tokens=7,
        transcript={"events": []},
    )


class ClaimValidationModeTest(unittest.TestCase):
    def _run(self, root, adapter, arm=toolsets.BASELINE, **kwargs):
        return run.run_task(
            fake_task(), arm, adapter, base_config(), corpus_root=root,
            records_path=os.path.join(root, "records.jsonl"),
            work_root=os.path.join(root, "work"),
            mode=run.CLAIM_VALIDATION_MODE, **kwargs,
        )

    def test_two_arms_share_complete_contract_except_tool_surface(self):
        with TemporaryDirectory() as root:
            forge_fixture(root)
            adapters = {
                arm: FakeAgent(claim_result())
                for arm in (toolsets.TREATMENT, toolsets.BASELINE)
            }
            run.run_task_both_arms(
                fake_task(), lambda arm: adapters[arm], base_config(),
                corpus_root=root, records_path=os.path.join(root, "records.jsonl"),
                work_root=os.path.join(root, "work"),
                mode=run.CLAIM_VALIDATION_MODE,
            )
            left, right = (adapters[arm].calls[0] for arm in
                           (toolsets.TREATMENT, toolsets.BASELINE))
            for field in left.__dataclass_fields__:
                if field == "allowed_tools":
                    self.assertNotEqual(getattr(left, field), getattr(right, field))
                else:
                    self.assertEqual(getattr(left, field), getattr(right, field), field)
            self.assertEqual(
                left.tool_instructions,
                toolsets.CLAIM_VALIDATION_INSTRUCTIONS,
            )
            self.assertIn('"verdict":"true|false|unsupported"', left.prompt)

    def test_structured_answer_and_sealed_record_metadata(self):
        with TemporaryDirectory() as root:
            forge_fixture(root)
            rec = self._run(root, FakeAgent(claim_result()))
        self.assertEqual(rec["status"], record.STATUS_ANSWERED)
        self.assertEqual(rec["structured_answer"]["verdict"], "false")
        self.assertEqual(rec["mode"], run.CLAIM_VALIDATION_MODE)
        self.assertEqual(rec["schema_version"], run.CLAIM_VALIDATION_SCHEMA)
        self.assertRegex(rec["claim_task_digest"], r"^sha256:[0-9a-f]{64}$")
        self.assertEqual(rec["sealed_metadata"], {
            "task_id": "pb-password-login-route", "category": None,
        })
        self.assertNotIn("task_id", rec)
        self.assertNotIn("task_digest", rec)

    def test_malformed_output_is_captured_and_terminal(self):
        with TemporaryDirectory() as root:
            forge_fixture(root)
            malformed = AgentResult("ok", "not json", (), 1, 2, {"raw": True})
            first = FakeAgent(malformed)
            rec = self._run(root, first)
            second = FakeAgent(claim_result())
            resumed = self._run(root, second)
        self.assertEqual(rec["status"], record.STATUS_MALFORMED_OUTPUT)
        self.assertEqual(rec["final_answer"], "not json")
        self.assertIsNone(rec["structured_answer"])
        self.assertTrue(rec["error"].startswith("malformed_claim_answer:"))
        self.assertTrue(resumed["skipped"])
        self.assertEqual(second.calls, [])

    def test_mode_and_schema_version_have_distinct_resume_keys(self):
        with TemporaryDirectory() as root:
            forge_fixture(root)
            self._run(root, FakeAgent(claim_result()), schema_version="claim_v1")
            second = FakeAgent(claim_result())
            rec = self._run(root, second, schema_version="claim_v2")
        self.assertFalse(rec.get("skipped", False))
        self.assertIn("mode-claim-validation::schema-claim_v2", rec["run_key"])

    def test_sensitive_reads_and_write_tools_fail_closed(self):
        with TemporaryDirectory() as root:
            forge_fixture(root)
            cases = (
                (ToolCall("Read", {"file_path": "annotations/key.json"}), "sealed_path"),
                (ToolCall("Read", {"file_path": "answer-keys/claim-one.json"}),
                 "sealed_path"),
                (ToolCall("Read", {"file_path":
                                   "benchmarks/repo-qa/exploration/tasks/nx-a.json"}),
                 "sealed_path"),
                (ToolCall("Write", {"file_path": "apis/base.go", "content": "x"}),
                 "tool_not_allowed"),
            )
            for index, (call, reason) in enumerate(cases):
                with self.subTest(call=call):
                    rec = self._run(
                        root, FakeAgent(claim_result((call,))),
                        schema_version=f"isolation-{index}",
                    )
                    self.assertEqual(rec["status"], record.STATUS_TOOL_VIOLATION)
                    self.assertEqual(rec["violations"][0]["reason"], reason)

    def test_structured_citations_reject_escape_and_sealed_paths(self):
        for path in ("../answer.json", "annotations/key.json", "/etc/passwd"):
            raw = json.dumps({
                "verdict": "true", "evidence": [{"path": path, "line": 1}],
                "rationale": "bad",
            })
            parsed, error = run._parse_claim_answer(raw)
            self.assertIsNone(parsed)
            self.assertEqual(error, "malformed_claim_answer: citation")


class SealedArtifactInventoryTest(unittest.TestCase):
    """Sealed material is identified by the location the harness owns, never by
    words in a filename: a corpus is free to ship files called `manifest`,
    `oracle` or `trap`, and two of the claim tasks depend on exactly such a
    file being readable and citable."""

    LEGITIMATE = (
        "packages/next/src/server/load-manifest.external.ts",
        "packages/next/src/build/manifests/write.ts",
        "packages/next/src/server/lib/trace/tracer.ts",
        "src/traps/trapdoor.py",
        "docs/annotations/README.md",
        "app/oracle_client.py",
        "manifest.json",
    )
    SEALED = (
        "annotations/key.json",
        "annotations",
        "annotations/nested/deep.json",
        "./annotations/key.json",
        "annotations\\key.json",
        "answer-keys/claim-one.json",
        "benchmarks/repo-qa/exploration/tasks/nx-a.json",
        "benchmarks/repo-qa/exploration/annotations/nx-a.ann-b.json",
        # The gate also serves the edit-mode arms; the edit bank's oracle
        # material is sealed inventory exactly like the exploration bank's.
        "benchmarks/repo-qa/edits/tasks/nx-retry-1.json",
        "benchmarks/repo-qa/edits/annotations/nx-retry-1.ann-hyde.json",
        "benchmarks/repo-qa/edits/schema/edit-task.schema.json",
        "benchmarks/repo-qa/edits/oracles/oracle_gate.py",
    )

    def test_legitimate_corpus_paths_are_not_sealed(self):
        with TemporaryDirectory() as checkout:
            for path in self.LEGITIMATE:
                with self.subTest(path=path):
                    self.assertFalse(gate._is_sealed_path(path, checkout))

    def test_harness_sealed_artifacts_are_still_sealed(self):
        with TemporaryDirectory() as checkout:
            for path in self.SEALED:
                with self.subTest(path=path):
                    self.assertTrue(gate._is_sealed_path(path, checkout))

    def test_absolute_in_checkout_spelling_is_sealed_like_the_relative_one(self):
        """An in-checkout ABSOLUTE path to sealed material must not bypass the
        check that flags the relative spelling of the very same file."""
        with TemporaryDirectory() as checkout:
            for rel in ("annotations/key.json",
                        "benchmarks/repo-qa/edits/tasks/nx-retry-1.json"):
                spellings = (rel, os.path.join(checkout, rel))
                for spelling in spellings:
                    with self.subTest(spelling=spelling):
                        violations = gate.enforce(
                            [ToolCall("Read", {"file_path": spelling})],
                            ["Read"], checkout,
                        )
                        self.assertEqual(
                            [v["reason"] for v in violations], ["sealed_path"]
                        )


class StaleForgeVersionCorpusTest(unittest.TestCase):
    """A stale corpus forged by an older forge is self-consistent (its tree
    matches its own manifest hash), so the tree-hash gate alone cannot reject
    it; the forge_version compat gate must."""

    def test_prepare_checkout_rejects_a_stale_forge_version_corpus(self):
        with TemporaryDirectory() as root:
            corpus_dir, tree, manifest = forge_fixture(root)
            stale = dict(manifest, forge_version="1")
            forge._write_json_atomic(
                os.path.join(corpus_dir, "manifest.json"), stale
            )
            with self.assertRaisesRegex(corpus.CorpusError, "forge_version"):
                corpus.prepare_checkout(
                    root, fake_task()["manifest_ref"], os.path.join(root, "co")
                )


class ConfigParityTest(unittest.TestCase):
    def test_agent_visible_task_contains_only_claim_and_request(self):
        task = fake_task()
        task["author"] = {"verdict": "false", "trap_id": "secret-trap"}
        task["evidence"] = [{"path": "secret/oracle.py"}]
        self.assertEqual(
            run.agent_visible_task(task),
            {"claim": task["claim"], "request": task["request"]},
        )

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

    def test_glob_pattern_escaping_checkout_is_rejected(self):
        with TemporaryDirectory() as checkout:
            violations = gate.enforce(
                [ToolCall("Glob", {"pattern": "../annotations/**/*.json"})],
                ("Glob",), checkout,
            )
        self.assertEqual(violations[0]["reason"], "path_escape")
        self.assertIn("pattern=", violations[0]["detail"])

    def test_malformed_and_unknown_arguments_are_rejected(self):
        with TemporaryDirectory() as checkout:
            malformed = gate.enforce(
                [ToolCall("Read", ["secret.py"])], ("Read",), checkout
            )
            unknown = gate.enforce(
                [ToolCall("Glob", {"pattern": "**/*.py", "cwd": "/tmp"})],
                ("Glob",), checkout,
            )
            missing = gate.enforce([ToolCall("Glob", {})], ("Glob",), checkout)
            bad_path = gate.enforce(
                [ToolCall("Grep", {"pattern": "x", "path": ["src"]})],
                ("Grep",), checkout,
            )
        self.assertEqual(malformed[0]["reason"], "malformed_arguments")
        self.assertEqual(unknown[0]["reason"], "unknown_argument")
        self.assertEqual(missing[0]["reason"], "malformed_arguments")
        self.assertEqual(bad_path[0]["reason"], "malformed_arguments")

    def test_existing_symlink_escape_is_rejected(self):
        with TemporaryDirectory() as checkout, TemporaryDirectory() as outside:
            os.symlink(outside, os.path.join(checkout, "escape"))
            violations = gate.enforce(
                [ToolCall("Read", {"file_path": "escape/answer.json"})],
                ("Read",), checkout,
            )
        self.assertEqual(violations[0]["reason"], "path_escape")

    def test_mcp_path_arguments_and_unknown_fields_fail_closed(self):
        cases = (
            ("mcp__lci__browse_file", {"file": "../answer.json"}, "path_escape"),
            ("mcp__lci__inspect_symbol", {"name": "x", "file": "/etc/passwd"}, "path_escape"),
            ("mcp__lci__find_files", {"pattern": "x", "directory": "../"}, "path_escape"),
            ("mcp__lci__search", {"pattern": "x", "cwd": "/tmp"}, "unknown_argument"),
            ("mcp__lci__list_symbols", {"kind": "all", "file": []}, "malformed_arguments"),
        )
        with TemporaryDirectory() as checkout:
            for name, arguments, reason in cases:
                with self.subTest(name=name, arguments=arguments):
                    violations = gate.enforce(
                        [ToolCall(name, arguments)], (name,), checkout
                    )
                    self.assertEqual(violations[0]["reason"], reason)


class ClaudeCliAdapterTest(unittest.TestCase):
    def request(self, checkout):
        return AgentRequest(
            model="claude-test-model", system_prompt="system", timeout_seconds=10,
            checkout_dir=checkout, allowed_tools=("Read",),
            tool_instructions="read only", prompt="find it",
        )

    @mock.patch("runner.adapter.subprocess.run")
    def test_stream_json_preserves_tool_history(self, run_mock):
        events = [
            {"type": "system", "subtype": "init"},
            {"type": "assistant", "message": {"content": [
                {"type": "tool_use", "name": "Read",
                 "input": {"file_path": "apis/base.go"}}
            ]}},
            {"type": "result", "subtype": "success", "result": "answer",
             "usage": {"input_tokens": 12, "output_tokens": 3}},
        ]
        run_mock.return_value = mock.Mock(
            returncode=0,
            stdout="\n".join(json.dumps(event) for event in events) + "\n",
            stderr="",
        )
        with TemporaryDirectory() as checkout:
            result = ClaudeCliAdapter().run(self.request(checkout))
        argv = run_mock.call_args.args[0]
        self.assertIn("stream-json", argv)
        self.assertIn("--verbose", argv)
        self.assertEqual(result.status_hint, "ok")
        self.assertEqual(result.tool_calls, (
            ToolCall("Read", {"file_path": "apis/base.go"}),
        ))
        self.assertEqual(result.final_answer, "answer")
        self.assertEqual(result.transcript, events)

    def test_missing_assistant_history_fails_closed(self):
        result = ClaudeCliAdapter._parse(
            json.dumps({"type": "result", "subtype": "success", "result": "answer"}),
            "",
        )
        self.assertEqual(result.status_hint, "provider_error")
        self.assertEqual(result.transcript["error"], "missing_tool_history")

    def test_malformed_assistant_history_fails_closed(self):
        malformed_messages = [
            {"content": "text"},
            {"content": [{"type": "tool_use", "name": "", "input": {}}]},
            {"content": [{"type": "tool_use", "name": "Read", "input": []}]},
            {"content": [{"type": "tool_use", "name": "Read"}]},
            {"content": ["not-a-block"]},
        ]
        for message in malformed_messages:
            with self.subTest(message=message):
                stdout = "\n".join(json.dumps(event) for event in (
                    {"type": "assistant", "message": message},
                    {"type": "result", "result": "answer"},
                ))
                result = ClaudeCliAdapter._parse(stdout, "")
                self.assertEqual(result.status_hint, "provider_error")
                self.assertEqual(
                    result.transcript["error"], "malformed_tool_history"
                )

    def test_invalid_result_event_fails_closed(self):
        valid_assistant = {
            "type": "assistant", "message": {"content": [
                {"type": "text", "text": "answer"}
            ]},
        }
        invalid_results = (
            {"type": "result", "subtype": "error", "result": "answer", "usage": {}},
            {"type": "result", "result": "answer", "usage": {}},
            {"type": "result", "subtype": "success", "result": "", "usage": {}},
            {"type": "result", "subtype": "success", "result": 7, "usage": {}},
            {"type": "result", "subtype": "success", "result": "answer", "usage": []},
            {"type": "result", "subtype": "success", "result": "answer",
             "usage": {"input_tokens": True}},
            {"type": "result", "subtype": "success", "result": "answer",
             "usage": {"input_tokens": "1"}},
            {"type": "result", "subtype": "success", "result": "answer",
             "usage": {"output_tokens": -1}},
        )
        for result_event in invalid_results:
            with self.subTest(result=result_event):
                stdout = "\n".join(json.dumps(event) for event in (
                    valid_assistant, result_event,
                ))
                result = ClaudeCliAdapter._parse(stdout, "")
                self.assertEqual(result.status_hint, "provider_error")
                self.assertEqual(result.transcript["error"], "invalid_cli_result")

    def test_valid_success_result_allows_absent_token_counters(self):
        events = (
            {"type": "assistant", "message": {"content": [
                {"type": "text", "text": "answer"}
            ]}},
            {"type": "result", "subtype": "success", "result": "answer",
             "usage": {}},
        )
        result = ClaudeCliAdapter._parse(
            "\n".join(json.dumps(event) for event in events), ""
        )
        self.assertEqual(result.status_hint, "ok")
        self.assertEqual((result.input_tokens, result.output_tokens), (0, 0))


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
            self.assertEqual(rec["task_digest"], task_digest(task))
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

    def test_prepare_checkout_rejects_escaping_symlink(self):
        with TemporaryDirectory() as root, TemporaryDirectory() as outside:
            corpus_dir, tree, manifest = forge_fixture(root)
            os.symlink(outside, os.path.join(tree, "escape"))
            manifest["tree_hash"] = forge.tree_hash(tree)
            forge._write_json_atomic(os.path.join(corpus_dir, "manifest.json"), manifest)
            with self.assertRaises(corpus.EscapingSymlink):
                corpus.prepare_checkout(
                    root, fake_task()["manifest_ref"], os.path.join(root, "co")
                )

    def test_prepare_checkout_rejects_absolute_symlink_within_source(self):
        with TemporaryDirectory() as root:
            corpus_dir, tree, manifest = forge_fixture(root)
            os.symlink(
                os.path.join(tree, "apis", "base.go"),
                os.path.join(tree, "absolute-link"),
            )
            manifest["tree_hash"] = forge.tree_hash(tree)
            forge._write_json_atomic(os.path.join(corpus_dir, "manifest.json"), manifest)
            with self.assertRaises(corpus.EscapingSymlink):
                corpus.prepare_checkout(
                    root, fake_task()["manifest_ref"], os.path.join(root, "co")
                )

    def test_prepare_checkout_preserves_contained_relative_symlink(self):
        with TemporaryDirectory() as root:
            corpus_dir, tree, manifest = forge_fixture(root)
            os.symlink("apis/base.go", os.path.join(tree, "base-link"))
            manifest["tree_hash"] = forge.tree_hash(tree)
            forge._write_json_atomic(os.path.join(corpus_dir, "manifest.json"), manifest)
            dest = os.path.join(root, "co")
            corpus.prepare_checkout(root, fake_task()["manifest_ref"], dest)
            self.assertTrue(os.path.islink(os.path.join(dest, "base-link")))
            self.assertEqual(os.readlink(os.path.join(dest, "base-link")), "apis/base.go")

    def test_prepare_checkout_validates_copied_destination(self):
        with TemporaryDirectory() as root:
            forge_fixture(root)
            with mock.patch(
                "runner.corpus.reject_escaping_symlinks",
                side_effect=(None, corpus.EscapingSymlink("late escape")),
            ) as reject:
                with self.assertRaises(corpus.EscapingSymlink):
                    corpus.prepare_checkout(
                        root, fake_task()["manifest_ref"], os.path.join(root, "co")
                    )
            self.assertEqual(reject.call_count, 2)
            self.assertFalse(os.path.exists(os.path.join(root, "co")))


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

    def test_changed_task_content_is_rerun_under_same_task_id(self):
        with TemporaryDirectory() as root:
            forge_fixture(root)
            task = fake_task()
            records_path = os.path.join(root, "records.jsonl")
            work = os.path.join(root, "work")
            run.run_task(
                task, toolsets.BASELINE, FakeAgent(answered_result([])), base_config(),
                corpus_root=root, records_path=records_path, work_root=work,
            )
            changed = dict(task)
            changed["prompt"] = task["prompt"] + " Also explain the call chain."
            second = FakeAgent(answered_result([]))
            rec = run.run_task(
                changed, toolsets.BASELINE, second, base_config(),
                corpus_root=root, records_path=records_path, work_root=work,
            )
            self.assertNotIn("skipped", rec)
            self.assertEqual(len(second.calls), 1)
            self.assertEqual(rec["task_digest"], task_digest(changed))
            self.assertEqual(len(record.load_records(records_path)), 2)

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
