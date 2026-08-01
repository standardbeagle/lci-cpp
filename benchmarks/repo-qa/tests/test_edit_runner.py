"""Contract tests for the Stage-3 EDIT-mode two-arm runner.

Hermetic + network-free: a tiny forged tree + manifest is synthesised in a
tempdir (reusing the forge's emit shape), the behaviour/existing-suite commands
are trivial real argv over a marker file, and a deterministic fake agent mutates
the checkout and yields canned tool calls / statuses. No real corpora, no model,
no credentials.

Acceptance criteria pinned here (one+ test each):
  1. the per-run record carries the mandated provenance (pinned commit/tree hash,
     mutation manifest ref, task/schema/gate versions, model, seed, arm/allowlist,
     patch hash, each gate outcome);
  2. identical config -> identical cell ordering + isolation setup; a completed
     cell is never re-run/duplicated on resume;
  3. NO oracle material (keys/tests/anchors) reaches the agent prompt OR the
     tool-visible tree;
  4. invalid patch / timeout / agent failure / gate failure each produce an
     explicit machine-readable outcome (no silent omission);
  5. a fake-agent smoke covers BOTH arms x ALL terminal outcome classes.
"""

import os
import sys
import unittest
from tempfile import TemporaryDirectory

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH_ROOT = os.path.dirname(HERE)
EXPLORATION_ROOT = os.path.join(BENCH_ROOT, "exploration")
SCRIPTS = os.path.join(BENCH_ROOT, "scripts")
EDITS_RUNNER = os.path.join(BENCH_ROOT, "edits", "runner")
for _p in (EDITS_RUNNER, EXPLORATION_ROOT, SCRIPTS):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import exploration_corpus_forge as forge  # noqa: E402
import edit_patch  # noqa: E402
import edit_record  # noqa: E402
import edit_run  # noqa: E402
import edit_toolsets  # noqa: E402
from runner.adapter import AgentResult, FakeAgent, ToolCall  # noqa: E402
from task_digest import task_digest  # noqa: E402

FAKE_COMMIT = "0" * 40

# A trivial, real behaviour command: RED (exit 1) on the pristine tree, GREEN
# (exit 0) once a "GREEN" marker file exists. The agent turns it green by
# writing that marker.
BEHAVIOR_CMD = [
    sys.executable, "-c",
    "import os,sys; sys.exit(0 if os.path.isfile('GREEN') else 1)",
]
SUITE_CMD = [sys.executable, "-c", "import sys; sys.exit(0)"]


def forge_fixture(root, corpus_id="pocketbase", seed=7):
    """Synthesise a forged corpus with two LIVE Go constructor exemplars that
    conform to the go-constructor-new-pointer rule (so the convention gate can
    pass), plus the manifest pinning the tree hash."""
    corpus_dir = os.path.join(root, corpus_id, f"seed-{seed}")
    tree = os.path.join(corpus_dir, "tree")
    os.makedirs(os.path.join(tree, "apis"))
    with open(os.path.join(tree, "apis", "base.go"), "w") as handle:
        handle.write(
            "package apis\n\n"
            "type Router struct{}\n\n"
            "func NewRouter() *Router { return &Router{} }\n"
        )
    with open(os.path.join(tree, "apis", "store.go"), "w") as handle:
        handle.write(
            "package apis\n\n"
            "type Store struct{}\n\n"
            "func NewStore() *Store { return &Store{} }\n"
        )
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


def edit_task(corpus_id="pocketbase", seed=7, task_id="pb-module-1"):
    return {
        "schema": "edit_task_v1",
        "id": task_id,
        "corpus": corpus_id,
        "category": "module-extraction-layout",
        "manifest_ref": {
            "corpus_id": corpus_id,
            "source_commit": FAKE_COMMIT,
            "seed": seed,
            "forge_version": forge.FORGE_VERSION,
        },
        "prompt": "Bring the off-pattern constructor in line with how its neighbours build their types.",
        "rubric": {
            "must_surface": ["returns a pointer from a New* constructor"],
            "answer_shape": "a small patch matching the house constructor style",
        },
        "behavior": {
            "command": ["pnpm", "test"],
            "assertion": "the constructor is exercised",
            "discrimination": {
                "red": "the sentinel value is returned",
                "green": "a pointer is returned by NewThing",
            },
        },
        "convention": {
            "rule_id": "go-constructor-new-pointer",
            "statement": "Constructors are named New<Type> and RETURN A POINTER *T.",
            "conforms_pattern": "func New\\w+\\([^)]*\\)\\s*\\*",
        },
        "exemplars": [
            {
                "path": "apis/base.go",
                "lines": [5],
                "claim": "NewRouter returns *Router",
                "target_identifiers": ["NewRouter"],
            },
            {
                "path": "apis/store.go",
                "lines": [5],
                "claim": "NewStore returns *Store",
                "target_identifiers": ["NewStore"],
            },
        ],
        "blast_radius": {"allow": ["GREEN", "apis/**", "widget/**"], "max_files": 3},
        "adjudication": {"annotators": ["ann-hyde", "ann-quill"], "resolved": True},
    }


def base_config():
    return edit_run.BaseConfig(
        model="claude-test-model",
        system_prompt=edit_toolsets.EDIT_SYSTEM_PROMPT,
        timeout_seconds=120,
    )


def _apply_and_answer(writes, tool_calls=(), status="ok", answer="done"):
    """Build a FakeAgent whose run() writes `writes` (relpath->text or None to
    delete) into the checkout, then returns a canned AgentResult."""

    def _run(request):
        for rel, content in writes.items():
            dest = os.path.join(request.checkout_dir, rel)
            if content is None:
                if os.path.isfile(dest):
                    os.remove(dest)
                continue
            os.makedirs(os.path.dirname(dest) or dest, exist_ok=True)
            with open(dest, "w", encoding="utf-8") as handle:
                handle.write(content)
        return AgentResult(
            status_hint=status, final_answer=answer,
            tool_calls=tuple(tool_calls), input_tokens=10, output_tokens=3,
            transcript={"turns": [{"role": "assistant", "text": answer}]},
        )

    return FakeAgent(_run)


# A conforming Go constructor: the agent's own edit must satisfy the task's
# convention rule (go-constructor-new-pointer), not merely leave the bank's
# pre-authored exemplars intact.
CONFORMING_EDIT = (
    "package apis\n\n"
    "type Widget struct{}\n\n"
    "func NewWidget() *Widget { return &Widget{} }\n"
)
# Same construct, returning a VALUE -- the rule's exact violation.
NON_CONFORMING_EDIT = (
    "package apis\n\n"
    "type Widget struct{}\n\n"
    "func NewWidget() Widget { return Widget{} }\n"
)


def run_pass_agent():
    # Turns behaviour red->green (the GREEN marker) AND makes a convention-
    # conforming edit, both inside the declared blast radius. Passing all three
    # gates requires both: the marker alone proves nothing about conventions.
    return _apply_and_answer(
        {"GREEN": "ok\n", "apis/widget.go": CONFORMING_EDIT}
    )


class ProvenanceRecordTest(unittest.TestCase):
    def test_passing_run_records_every_mandated_field(self):
        with TemporaryDirectory() as root:
            _, _, manifest = forge_fixture(root)
            task = edit_task()
            records = os.path.join(root, "records.jsonl")
            work = os.path.join(root, "work")
            rec = edit_run.run_edit_task(
                task, edit_toolsets.TREATMENT, run_pass_agent(), base_config(),
                corpus_root=root, records_path=records, work_root=work,
                behavior_command=BEHAVIOR_CMD, existing_suite_command=SUITE_CMD,
            )
            self.assertEqual(rec["status"], edit_record.STATUS_PASSED)
            # pinned source commit + tree hash
            self.assertEqual(rec["source_commit"], FAKE_COMMIT)
            self.assertEqual(rec["manifest_id"], manifest["tree_hash"])
            # mutation manifest reference
            self.assertEqual(rec["manifest_ref"]["corpus_id"], "pocketbase")
            self.assertEqual(rec["manifest_ref"]["forge_version"], forge.FORGE_VERSION)
            # task / schema / gate versions
            self.assertEqual(rec["task_digest"], task_digest(task))
            self.assertEqual(rec["task_schema"], "edit_task_v1")
            self.assertEqual(rec["gate_versions"]["oracle"], "oracle_gate_v1")
            self.assertEqual(rec["gate_versions"]["conformance"], "conformance_gate_v1")
            # model + seed + arm/allowlist
            self.assertEqual(rec["model"], "claude-test-model")
            self.assertEqual(rec["seed"], 7)
            self.assertEqual(rec["arm"], edit_toolsets.TREATMENT)
            self.assertEqual(
                rec["effective_allowlist"], list(edit_toolsets.TREATMENT_TOOLS)
            )
            self.assertIn("Edit", rec["effective_allowlist"])
            self.assertIn("Write", rec["effective_allowlist"])
            # patch hash + touched files
            self.assertTrue(rec["patch_hash"].startswith("sha256:"))
            self.assertEqual(rec["patch_files"], ["GREEN"])
            # each gate outcome present
            self.assertTrue(rec["gates"]["oracle"]["passed"])
            self.assertTrue(rec["gates"]["conformance"]["passed"])
            self.assertEqual(rec["gates"]["oracle"]["schema"], "oracle_gate_v1")
            self.assertEqual(
                rec["gates"]["conformance"]["schema"], "conformance_gate_v1"
            )
            for field in ("started_at", "ended_at"):
                self.assertTrue(rec[field])
            self.assertTrue(os.path.isfile(rec["transcript_ref"]))

    def test_isolated_worktree_is_cleaned_and_source_never_mutated(self):
        with TemporaryDirectory() as root:
            _, tree, manifest = forge_fixture(root)
            pre_hash = manifest["tree_hash"]
            task = edit_task()
            work = os.path.join(root, "work")
            edit_run.run_edit_task(
                task, edit_toolsets.TREATMENT, run_pass_agent(), base_config(),
                corpus_root=root, records_path=os.path.join(root, "r.jsonl"),
                work_root=work, behavior_command=BEHAVIOR_CMD,
                existing_suite_command=SUITE_CMD,
            )
            # source corpus tree untouched
            self.assertEqual(forge.tree_hash(tree), pre_hash)
            # throwaway checkout removed
            checkouts = os.path.join(work, "checkouts")
            leftover = os.listdir(checkouts) if os.path.isdir(checkouts) else []
            self.assertEqual(leftover, [])


class OracleIsolationTest(unittest.TestCase):
    """Criterion 3: no oracle key/test/anchor reaches the prompt or the tree."""

    def _oracle_terms(self, task):
        terms = []
        for ex in task["exemplars"]:
            terms.append(ex["path"])
            terms.extend(ex["target_identifiers"])
            terms.append(ex["claim"])
        terms.append(task["convention"]["conforms_pattern"])
        terms.append(task["convention"]["statement"])
        terms.append(task["convention"]["rule_id"])
        terms.append(task["behavior"]["discrimination"]["red"])
        terms.append(task["behavior"]["discrimination"]["green"])
        terms.append(task["behavior"]["assertion"])
        terms.extend(task["rubric"]["must_surface"])
        terms.append(task["rubric"]["answer_shape"])
        return terms

    def test_agent_prompt_carries_no_oracle_material(self):
        task = edit_task()
        base = base_config()
        for arm in (edit_toolsets.TREATMENT, edit_toolsets.BASELINE):
            request = edit_toolsets.build_request(
                base, arm, "/tmp/checkout-x", task["prompt"]
            )
            visible = "\n".join(
                (request.system_prompt, request.tool_instructions, request.prompt)
            )
            self.assertEqual(request.prompt, task["prompt"])
            for term in self._oracle_terms(task):
                self.assertNotIn(term, visible, f"oracle term leaked: {term!r}")

    def test_tool_visible_tree_holds_no_injected_oracle_file(self):
        # The checkout the agent sees must be exactly the pristine corpus files;
        # the harness injects no discrimination test / anchor / oracle file.
        with TemporaryDirectory() as root:
            _, tree, _ = forge_fixture(root)
            task = edit_task()
            captured = {}

            def _snoop(request):
                captured["files"] = edit_patch.agent_visible_files(
                    request.checkout_dir
                )
                captured["prompt"] = request.prompt
                return AgentResult("ok", "done", (), 0, 0, {})

            edit_run.run_edit_task(
                task, edit_toolsets.BASELINE, FakeAgent(_snoop), base_config(),
                corpus_root=root, records_path=os.path.join(root, "r.jsonl"),
                work_root=os.path.join(root, "work"),
                behavior_command=BEHAVIOR_CMD, existing_suite_command=SUITE_CMD,
            )
            self.assertEqual(
                captured["files"], edit_patch.agent_visible_files(tree)
            )
            for term in self._oracle_terms(task):
                self.assertNotIn(term, captured["prompt"])


class DeterminismAndResumeTest(unittest.TestCase):
    def test_bank_cell_order_is_deterministic(self):
        with TemporaryDirectory() as root:
            forge_fixture(root, corpus_id="pocketbase", seed=7)
            forge_fixture(root, corpus_id="scikit-learn", seed=7)
            tasks = [
                edit_task(task_id="pb-module-1"),
                edit_task(
                    corpus_id="scikit-learn", task_id="skl-module-1"
                ),
            ]
            work = os.path.join(root, "work")

            def factory(_arm):
                return _apply_and_answer({"GREEN": "ok\n"})

            keys = [
                rec["run_key"]
                for rec in edit_run.run_edit_bank(
                    tasks, factory, base_config(),
                    corpus_root=root,
                    records_path=os.path.join(root, "r.jsonl"),
                    work_root=work, behavior_command=BEHAVIOR_CMD,
                    existing_suite_command=SUITE_CMD,
                )
            ]
            self.assertEqual(
                keys,
                [
                    "pb-module-1::treatment::seed-7",
                    "pb-module-1::baseline::seed-7",
                    "skl-module-1::treatment::seed-7",
                    "skl-module-1::baseline::seed-7",
                ],
            )

    def test_completed_cell_is_not_rerun(self):
        with TemporaryDirectory() as root:
            forge_fixture(root)
            task = edit_task()
            records = os.path.join(root, "records.jsonl")
            work = os.path.join(root, "work")
            edit_run.run_edit_task(
                task, edit_toolsets.TREATMENT, run_pass_agent(), base_config(),
                corpus_root=root, records_path=records, work_root=work,
                behavior_command=BEHAVIOR_CMD, existing_suite_command=SUITE_CMD,
            )
            second = _apply_and_answer({"GREEN": "ok\n"})
            rec = edit_run.run_edit_task(
                task, edit_toolsets.TREATMENT, second, base_config(),
                corpus_root=root, records_path=records, work_root=work,
                behavior_command=BEHAVIOR_CMD, existing_suite_command=SUITE_CMD,
            )
            self.assertTrue(rec["skipped"])
            self.assertEqual(second.calls, [])  # adapter never touched on resume
            key = edit_record.run_key(task["id"], edit_toolsets.TREATMENT, 7)
            persisted = [
                r["run_key"] for r in edit_record.load_records(records)
            ]
            self.assertEqual(persisted.count(key), 1)

    def test_changed_task_content_reruns_under_same_id(self):
        with TemporaryDirectory() as root:
            forge_fixture(root)
            task = edit_task()
            records = os.path.join(root, "records.jsonl")
            work = os.path.join(root, "work")
            edit_run.run_edit_task(
                task, edit_toolsets.TREATMENT, run_pass_agent(), base_config(),
                corpus_root=root, records_path=records, work_root=work,
                behavior_command=BEHAVIOR_CMD, existing_suite_command=SUITE_CMD,
            )
            changed = dict(task)
            changed["prompt"] = task["prompt"] + " Also keep the receiver name."
            second = run_pass_agent()
            rec = edit_run.run_edit_task(
                changed, edit_toolsets.TREATMENT, second, base_config(),
                corpus_root=root, records_path=records, work_root=work,
                behavior_command=BEHAVIOR_CMD, existing_suite_command=SUITE_CMD,
            )
            self.assertNotIn("skipped", rec)
            self.assertEqual(len(second.calls), 1)
            self.assertEqual(len(edit_record.load_records(records)), 2)

    def test_retryable_failure_is_rerun(self):
        with TemporaryDirectory() as root:
            forge_fixture(root)
            task = edit_task()
            records = os.path.join(root, "records.jsonl")
            work = os.path.join(root, "work")
            failed = FakeAgent(AgentResult("provider_error", None, (), 0, 0, {}))
            first = edit_run.run_edit_task(
                task, edit_toolsets.TREATMENT, failed, base_config(),
                corpus_root=root, records_path=records, work_root=work,
                behavior_command=BEHAVIOR_CMD, existing_suite_command=SUITE_CMD,
            )
            self.assertEqual(first["status"], edit_record.STATUS_AGENT_FAILURE)
            retry = run_pass_agent()
            rec = edit_run.run_edit_task(
                task, edit_toolsets.TREATMENT, retry, base_config(),
                corpus_root=root, records_path=records, work_root=work,
                behavior_command=BEHAVIOR_CMD, existing_suite_command=SUITE_CMD,
            )
            self.assertFalse(rec.get("skipped"))
            self.assertEqual(rec["status"], edit_record.STATUS_PASSED)
            self.assertEqual(len(retry.calls), 1)


# ---------------------------------------------------------------------------
# criterion 4 + 5: every terminal outcome class, BOTH arms, fake-agent smoke
# ---------------------------------------------------------------------------


def _agent_for(kind):
    """A fake agent that drives `run_edit_task` to a specific terminal class."""
    if kind == edit_record.STATUS_PASSED:
        return _apply_and_answer({"GREEN": "ok\n"})
    if kind == edit_record.STATUS_GATE_FAILED:
        # edits an allowed file but never turns the behaviour green -> the
        # behaviour discrimination fails (both trees red).
        return _apply_and_answer({"apis/base.go": "package apis\n// touched\n"})
    if kind == edit_record.STATUS_INVALID_PATCH:
        return _apply_and_answer({})  # no edit at all -> empty patch
    if kind == edit_record.STATUS_TOOL_VIOLATION:
        # reaches outside the checkout -> path_escape, both arms.
        return _apply_and_answer(
            {"GREEN": "ok\n"},
            tool_calls=[ToolCall("Read", {"file_path": "../../../etc/passwd"})],
        )
    if kind == edit_record.STATUS_TIMEOUT:
        return FakeAgent(AgentResult("timeout", None, (), 0, 0, {"e": "deadline"}))
    if kind == edit_record.STATUS_AGENT_FAILURE:
        return FakeAgent(AgentResult("provider_error", None, (), 0, 0, {"e": 1}))
    raise AssertionError(f"no smoke agent for {kind}")


class FakeAgentSmokeTest(unittest.TestCase):
    TERMINAL_CLASSES = (
        edit_record.STATUS_PASSED,
        edit_record.STATUS_GATE_FAILED,
        edit_record.STATUS_INVALID_PATCH,
        edit_record.STATUS_TOOL_VIOLATION,
        edit_record.STATUS_TIMEOUT,
        edit_record.STATUS_AGENT_FAILURE,
        edit_record.STATUS_CONFIG_ERROR,
    )

    def test_both_arms_reach_every_terminal_class(self):
        for arm in (edit_toolsets.TREATMENT, edit_toolsets.BASELINE):
            for kind in self.TERMINAL_CLASSES:
                with self.subTest(arm=arm, kind=kind):
                    with TemporaryDirectory() as root:
                        records = os.path.join(root, "records.jsonl")
                        work = os.path.join(root, "work")
                        if kind == edit_record.STATUS_CONFIG_ERROR:
                            # corpus absent -> config error, agent never invoked.
                            agent = FakeAgent(AgentResult("ok", "x", (), 0, 0, {}))
                            rec = edit_run.run_edit_task(
                                edit_task(), arm, agent, base_config(),
                                corpus_root=root, records_path=records,
                                work_root=work, behavior_command=BEHAVIOR_CMD,
                                existing_suite_command=SUITE_CMD,
                            )
                            self.assertEqual(agent.calls, [])
                        else:
                            forge_fixture(root)
                            rec = edit_run.run_edit_task(
                                edit_task(), arm, _agent_for(kind), base_config(),
                                corpus_root=root, records_path=records,
                                work_root=work, behavior_command=BEHAVIOR_CMD,
                                existing_suite_command=SUITE_CMD,
                            )
                        self.assertEqual(rec["status"], kind)
                        # every terminal outcome is explicit + machine-readable
                        self.assertIn(rec["status"], edit_record.ALL_STATUSES)
                        self.assertIsNotNone(
                            rec.get("reason") or rec.get("error"),
                            "terminal outcome must carry a reason/error",
                        )

    def test_unexpected_exception_is_recorded_before_it_propagates(self):
        # run_edit_task promises to append EXACTLY ONE provenance-complete record
        # per cell. Any unexpected exception raised after prepare_checkout used
        # to skip _finish entirely: the bank drain aborted with NOTHING written,
        # so the cell was invisible in the log -- indistinguishable from a cell
        # that was never planned, and silently absent from the scorer's
        # denominator story. Fail fast, but RECORD FIRST.
        class Boom(RuntimeError):
            pass

        def _explode(_request):
            raise Boom("adapter died mid-run")

        with TemporaryDirectory() as root:
            forge_fixture(root)
            records = os.path.join(root, "records.jsonl")
            work = os.path.join(root, "work")
            with self.assertRaises(Boom):
                edit_run.run_edit_task(
                    edit_task(), edit_toolsets.TREATMENT, FakeAgent(_explode),
                    base_config(), corpus_root=root, records_path=records,
                    work_root=work, behavior_command=BEHAVIOR_CMD,
                    existing_suite_command=SUITE_CMD,
                )
            persisted = edit_record.load_records(records)
            self.assertEqual(len(persisted), 1)
            rec = persisted[0]
            self.assertEqual(rec["status"], edit_record.STATUS_HARNESS_FAILURE)
            self.assertEqual(rec["reason"], edit_run.REASON_HARNESS_ERROR)
            # the exception class AND message are both in the recorded detail
            self.assertIn("Boom", rec["error"])
            self.assertIn("adapter died mid-run", rec["error"])
            # provenance is complete despite the crash
            self.assertEqual(rec["task_id"], "pb-module-1")
            self.assertEqual(rec["arm"], edit_toolsets.TREATMENT)
            self.assertEqual(rec["seed"], 7)
            self.assertEqual(rec["source_commit"], FAKE_COMMIT)
            self.assertEqual(rec["run_key"], "pb-module-1::treatment::seed-7")
            # a harness crash is NOT terminal: resume must retry the cell
            self.assertIn(
                edit_record.STATUS_HARNESS_FAILURE, edit_record.RETRYABLE_STATUSES
            )
            # and the throwaway checkout is still cleaned up
            checkouts = os.path.join(work, "checkouts")
            self.assertEqual(
                os.listdir(checkouts) if os.path.isdir(checkouts) else [], []
            )

    def test_gate_exception_is_recorded_before_it_propagates(self):
        # Same contract for a fault raised by the GATES rather than the adapter.
        import conformance_gate

        original = conformance_gate.evaluate_task

        def _explode(*_a, **_kw):
            raise ValueError("gate wiring is wrong")

        with TemporaryDirectory() as root:
            forge_fixture(root)
            records = os.path.join(root, "records.jsonl")
            conformance_gate.evaluate_task = _explode
            try:
                with self.assertRaises(ValueError):
                    edit_run.run_edit_task(
                        edit_task(), edit_toolsets.BASELINE, run_pass_agent(),
                        base_config(), corpus_root=root, records_path=records,
                        work_root=os.path.join(root, "work"),
                        behavior_command=BEHAVIOR_CMD,
                        existing_suite_command=SUITE_CMD,
                    )
            finally:
                conformance_gate.evaluate_task = original
            persisted = edit_record.load_records(records)
            self.assertEqual(len(persisted), 1)
            self.assertEqual(
                persisted[0]["status"], edit_record.STATUS_HARNESS_FAILURE
            )
            self.assertIn("gate wiring is wrong", persisted[0]["error"])

    def test_convention_gate_judges_the_agents_patch_not_the_bank(self):
        # Three agents, identical except for what they wrote. The convention
        # verdict must separate them. Before this, all three passed convention:
        # the gate re-verified the task's own exemplar anchors, which conform in
        # the pristine tree and still conform afterwards regardless of the agent.
        cases = {
            # behaviour green, convention UNPROVEN (touched nothing governed)
            "marker_only": ({"GREEN": "ok\n"}, False),
            # behaviour green + a conforming constructor
            "conforming": (
                {"GREEN": "ok\n", "apis/widget.go": CONFORMING_EDIT}, True
            ),
            # behaviour green + a New* constructor returning a value
            "non_conforming": (
                {"GREEN": "ok\n", "apis/widget.go": NON_CONFORMING_EDIT}, False
            ),
        }
        for label, (writes, expected) in sorted(cases.items()):
            with self.subTest(agent=label):
                with TemporaryDirectory() as root:
                    forge_fixture(root)
                    rec = edit_run.run_edit_task(
                        edit_task(), edit_toolsets.TREATMENT,
                        _apply_and_answer(writes), base_config(),
                        corpus_root=root,
                        records_path=os.path.join(root, "r.jsonl"),
                        work_root=os.path.join(root, "work"),
                        behavior_command=BEHAVIOR_CMD,
                        existing_suite_command=SUITE_CMD,
                    )
                    conformance = rec["gates"]["conformance"]
                    # behaviour is green in all three -- only convention differs
                    self.assertTrue(rec["gates"]["oracle"]["passed"],
                                    msg=rec["gates"]["oracle"]["diagnostic"])
                    self.assertEqual(
                        conformance["passed"], expected, msg=conformance["diagnostic"]
                    )
                    # the bank's exemplars are healthy in EVERY case, so they
                    # cannot be what the verdict is tracking.
                    self.assertTrue(conformance["bank_health"]["passed"])
                    self.assertEqual(
                        rec["status"],
                        edit_record.STATUS_PASSED if expected
                        else edit_record.STATUS_GATE_FAILED,
                    )

    def test_blast_radius_escape_is_a_gate_failure(self):
        # A patch that turns behaviour green but ALSO writes outside the declared
        # blast radius is rejected by the changed-path gate (criterion 4).
        with TemporaryDirectory() as root:
            forge_fixture(root)
            agent = _apply_and_answer({"GREEN": "ok\n", "core/db.go": "x\n"})
            rec = edit_run.run_edit_task(
                edit_task(), edit_toolsets.TREATMENT, agent, base_config(),
                corpus_root=root, records_path=os.path.join(root, "r.jsonl"),
                work_root=os.path.join(root, "work"),
                behavior_command=BEHAVIOR_CMD, existing_suite_command=SUITE_CMD,
            )
            self.assertEqual(rec["status"], edit_record.STATUS_GATE_FAILED)
            self.assertFalse(rec["gates"]["oracle"]["changed_path"]["passed"])
            self.assertEqual(
                rec["gates"]["oracle"]["changed_path"]["out_of_scope"],
                ["core/db.go"],
            )


if __name__ == "__main__":
    unittest.main()
