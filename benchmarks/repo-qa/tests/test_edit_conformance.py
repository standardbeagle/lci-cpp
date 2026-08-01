#!/usr/bin/env python3
"""Unit tests for the stage-3 mechanical convention-conformance GATE
(``edits/conformance/conformance_gate.py``).

Hermetic: every test builds a synthetic forged corpus (a tiny mutated tree plus
a manifest in the shape the forge emits, including a decoy twin) in a tempdir.
Nothing here touches the network or the multi-gigabyte real corpora, so the
suite runs anywhere via::

    python3 -m unittest discover -s benchmarks/repo-qa/tests

Coverage map:

  * Discrimination (criteria 1, 2): for EACH supported rule kind -- one per
    convention family from S3.1 -- a conforming POSITIVE anchor PASSES, a
    pre-edit off-pattern NEGATIVE anchor FAILS, and a dead/deprecated DECOY
    twin FAILS. No happy-path-only proof.
  * Fail closed (criterion 3): a stable reason code is asserted for a missing
    anchor, a non-live anchor, a stale line bound, an absent identifier, an
    ambiguous (>1) match, a malformed rule, an unsupported rule, an absent
    manifest, and a tool failure.
  * Oracle independence: a task whose authored ``conforms_pattern`` is sabotaged
    to accept the broken form is still judged by structure, proving the gate
    never re-runs the authoring regex.
  * Determinism (criterion 4): outcomes are byte-stable and anchor-sorted.
"""

import json
import os
import sys
import tempfile
import unittest

CONFORMANCE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "edits",
    "conformance",
)
sys.path.insert(0, CONFORMANCE)

import conformance_gate as gate  # noqa: E402

CORPUS_ID = "pocketbase"
SEED = 77

# ---------------------------------------------------------------------------
# synthetic corpus bodies, one per supported rule family. Each carries a
# conforming positive site, a pre-edit off-pattern negative site, and (via the
# decoy) a dead twin.
# ---------------------------------------------------------------------------

TS_THROW = "\n".join([
    "export function extract(node) {",          # 1
    "  if (node.kind === 'ok') {",              # 2
    "    return node.value",                     # 3
    "  }",                                       # 4
    "  throw new ParseError('unexpected node')", # 5  positive
    "}",                                         # 6
    "",                                          # 7
    "export function legacyExtract(node) {",     # 8
    "  return null",                             # 9  negative (sentinel)
    "}",                                         # 10
]) + "\n"

TS_LOG = "\n".join([
    "export function warnDeprecated(msg) {",     # 1
    "  Log.warn(msg)",                           # 2  positive
    "}",                                         # 3
    "",                                          # 4
    "export function warnLegacy(msg) {",         # 5
    "  console.warn(msg)",                       # 6  negative (raw console)
    "}",                                         # 7
    "",                                          # 8
    "export function warnTwice(a, b) {",         # 9
    "  Log.warn(a)",                             # 10 ambiguous with 11
    "  Log.error(b)",                            # 11
    "}",                                         # 12
]) + "\n"

GO_CTOR = "\n".join([
    "package core",                              # 1
    "",                                          # 2
    "type Store struct{}",                       # 3
    "",                                          # 4
    "func NewStore(cfg Config) *Store {",        # 5  positive (pointer ctor)
    "\treturn &Store{}",                         # 6
    "}",                                         # 7
    "",                                          # 8
    "func BuildCache() Cache {",                 # 9  negative (not a New ctor)
    "\treturn Cache{}",                          # 10
    "}",                                         # 11
    "",                                          # 12
    "func NewValue() Config {",                  # 13 negative (New but no pointer)
    "\treturn Config{}",                         # 14
    "}",                                         # 15
]) + "\n"

GO_HANDLER = "\n".join([
    "package apis",                                          # 1
    "",                                                      # 2
    "func handleList(e *core.RequestEvent) error {",         # 3  positive
    "\treturn e.JSON(200, nil)",                             # 4
    "}",                                                     # 5
    "",                                                      # 6
    "func handleLegacy(w http.ResponseWriter, r *http.Request) {",  # 7 negative
    "\tw.WriteHeader(200)",                                  # 8
    "}",                                                     # 9
]) + "\n"


def _write(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(obj if isinstance(obj, str) else json.dumps(obj, indent=2))


class Corpus:
    """A synthetic forged tree + manifest. ``live`` files land in path_map;
    ``decoys`` land in the decoy list (and, unless suppressed, on disk too)."""

    def __init__(self, root, live, decoys=None, drop_from_disk=()):
        self.root = root
        seed_dir = os.path.join(root, CORPUS_ID, f"seed-{SEED}")
        self.tree_dir = os.path.join(seed_dir, "tree")
        decoys = decoys or {}
        for rel, body in live.items():
            _write(os.path.join(self.tree_dir, rel), body)
        for rel, body in decoys.items():
            _write(os.path.join(self.tree_dir, rel), body)
        for rel in drop_from_disk:
            path = os.path.join(self.tree_dir, rel)
            if os.path.isfile(path):
                os.remove(path)
        self.manifest = {
            "schema": "exploration_corpus_manifest_v1",
            "corpus_id": CORPUS_ID,
            "source_commit": "0" * 40,
            "seed": SEED,
            "forge_version": gate.vet.forge.FORGE_VERSION,
            "path_map": {rel: rel for rel in live},
            "decoys": [
                {"path": rel, "derived_from": next(iter(live))} for rel in decoys
            ],
            "tree_hash": "0" * 64,
            "status": "ready",
        }
        _write(os.path.join(seed_dir, "manifest.json"), self.manifest)


def _task(rule_id, exemplars, conforms_pattern="UNUSED-BY-GATE"):
    return {
        "schema": "edit_task_v1",
        "id": f"t-{rule_id}",
        "corpus": CORPUS_ID,
        "manifest_ref": {"corpus_id": CORPUS_ID, "seed": SEED},
        "convention": {
            "rule_id": rule_id,
            "statement": "x" * 12,
            "conforms_pattern": conforms_pattern,
        },
        "exemplars": exemplars,
    }


def _anchor(path, lines, identifiers):
    return {
        "path": path,
        "lines": list(lines),
        "claim": "demonstrates the convention",
        "target_identifiers": list(identifiers),
    }


class GateTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self._tmp.cleanup()

    def corpus(self, live, **kw):
        return Corpus(self._tmp.name, live, **kw)

    def reasons(self, outcome):
        return [a["reason"] for a in outcome["anchors"]]


# ---------------------------------------------------------------------------
# discrimination per supported rule family (criteria 1, 2)
# ---------------------------------------------------------------------------


class DiscriminationTest(GateTestBase):
    def _discriminate(self, rule_id, rel, body, pos, neg, ident_pos, ident_neg,
                      decoy_rel):
        corpus = self.corpus(
            {rel: body}, decoys={decoy_rel: body},
        )
        # positive conforms
        out = gate.evaluate_task(
            _task(rule_id, [_anchor(rel, pos, ident_pos)]),
            corpus.manifest, corpus.tree_dir,
        )
        self.assertTrue(out["passed"], msg=out["diagnostic"])
        self.assertEqual(self.reasons(out), [gate.Reason.CONFORMS])
        # negative (pre-edit off-pattern) fails
        out = gate.evaluate_task(
            _task(rule_id, [_anchor(rel, neg, ident_neg)]),
            corpus.manifest, corpus.tree_dir,
        )
        self.assertFalse(out["passed"])
        self.assertEqual(self.reasons(out), [gate.Reason.MATCH_ABSENT])
        # dead/deprecated twin cannot satisfy the gate
        out = gate.evaluate_task(
            _task(rule_id, [_anchor(decoy_rel, pos, ident_pos)]),
            corpus.manifest, corpus.tree_dir,
        )
        self.assertFalse(out["passed"])
        self.assertEqual(self.reasons(out), [gate.Reason.ANCHOR_DECOY])

    def test_retry_error_handling_throws_typed_error(self):
        self._discriminate(
            "ts-throw-typed-error", "pkg/analysis.ts", TS_THROW,
            pos=[5], neg=[8, 9], ident_pos=["ParseError"], ident_neg=["legacyExtract"],
            decoy_rel="pkg/analysis_dead.ts",
        )

    def test_logging_central_log_namespace(self):
        self._discriminate(
            "ts-central-log-namespace", "pkg/log.ts", TS_LOG,
            pos=[2], neg=[6], ident_pos=["Log"], ident_neg=["console"],
            decoy_rel="pkg/log_dead.ts",
        )

    def test_module_layout_constructor_new_pointer(self):
        self._discriminate(
            "go-constructor-new-pointer", "core/base.go", GO_CTOR,
            pos=[5], neg=[9], ident_pos=["NewStore"], ident_neg=["BuildCache"],
            decoy_rel="core/base_dead.go",
        )

    def test_api_shape_request_event_handler(self):
        self._discriminate(
            "go-request-event-handler-shape", "apis/route.go", GO_HANDLER,
            pos=[3], neg=[7], ident_pos=["handleList"], ident_neg=["handleLegacy"],
            decoy_rel="apis/route_dead.go",
        )

    def test_constructor_without_pointer_return_fails(self):
        # Extra pointer-clause proof: `func NewValue()` returns a value, not a
        # pointer -> the structural predicate rejects it (regex `^func New\w+\(`
        # would have accepted it).
        corpus = self.corpus({"core/base.go": GO_CTOR})
        out = gate.evaluate_task(
            _task("go-constructor-new-pointer",
                  [_anchor("core/base.go", [13], ["NewValue"])]),
            corpus.manifest, corpus.tree_dir,
        )
        self.assertEqual(self.reasons(out), [gate.Reason.MATCH_ABSENT])


# ---------------------------------------------------------------------------
# fail-closed reason codes (criterion 3)
# ---------------------------------------------------------------------------


class FailClosedTest(GateTestBase):
    def _log_corpus(self):
        return self.corpus(
            {"pkg/log.ts": TS_LOG}, decoys={"pkg/log_dead.ts": TS_LOG},
        )

    def test_decoy_anchor_fails_closed(self):
        c = self._log_corpus()
        out = gate.evaluate_task(
            _task("ts-central-log-namespace",
                  [_anchor("pkg/log_dead.ts", [2], ["Log"])]),
            c.manifest, c.tree_dir,
        )
        self.assertEqual(self.reasons(out), [gate.Reason.ANCHOR_DECOY])

    def test_non_live_anchor_fails_closed(self):
        c = self._log_corpus()
        out = gate.evaluate_task(
            _task("ts-central-log-namespace",
                  [_anchor("pkg/not_in_manifest.ts", [2], ["Log"])]),
            c.manifest, c.tree_dir,
        )
        self.assertEqual(self.reasons(out), [gate.Reason.ANCHOR_NOT_LIVE])

    def test_missing_file_fails_closed(self):
        # path is live in the manifest but the file is not on disk.
        c = self.corpus({"pkg/log.ts": TS_LOG}, drop_from_disk=["pkg/log.ts"])
        out = gate.evaluate_task(
            _task("ts-central-log-namespace",
                  [_anchor("pkg/log.ts", [2], ["Log"])]),
            c.manifest, c.tree_dir,
        )
        self.assertEqual(self.reasons(out), [gate.Reason.ANCHOR_MISSING])

    def test_stale_line_bound_fails_closed(self):
        c = self._log_corpus()
        out = gate.evaluate_task(
            _task("ts-central-log-namespace",
                  [_anchor("pkg/log.ts", [2, 999], ["Log"])]),
            c.manifest, c.tree_dir,
        )
        self.assertEqual(self.reasons(out), [gate.Reason.ANCHOR_BOUND_STALE])

    def test_absent_identifier_fails_closed(self):
        c = self._log_corpus()
        out = gate.evaluate_task(
            _task("ts-central-log-namespace",
                  [_anchor("pkg/log.ts", [2], ["NoSuchSymbol"])]),
            c.manifest, c.tree_dir,
        )
        self.assertEqual(self.reasons(out), [gate.Reason.ANCHOR_IDENTIFIER_ABSENT])

    def test_ambiguous_match_fails_closed(self):
        c = self._log_corpus()
        out = gate.evaluate_task(
            _task("ts-central-log-namespace",
                  [_anchor("pkg/log.ts", [10, 11], ["Log"])]),
            c.manifest, c.tree_dir,
        )
        self.assertEqual(self.reasons(out), [gate.Reason.MATCH_AMBIGUOUS])

    def test_malformed_rule_fails_closed(self):
        c = self._log_corpus()
        task = _task("ts-central-log-namespace",
                     [_anchor("pkg/log.ts", [2], ["Log"])])
        del task["convention"]["rule_id"]
        out = gate.evaluate_task(task, c.manifest, c.tree_dir)
        self.assertFalse(out["passed"])
        self.assertIn(gate.Reason.RULE_MALFORMED, out["diagnostic"])

    def test_unsupported_rule_fails_closed(self):
        c = self._log_corpus()
        out = gate.evaluate_task(
            _task("zz-no-such-rule", [_anchor("pkg/log.ts", [2], ["Log"])]),
            c.manifest, c.tree_dir,
        )
        self.assertFalse(out["passed"])
        self.assertIn(gate.Reason.RULE_UNSUPPORTED, out["diagnostic"])

    def test_absent_manifest_fails_closed(self):
        # evaluate_task_in_corpus with no forged tree present.
        task = _task("ts-central-log-namespace",
                     [_anchor("pkg/log.ts", [2], ["Log"])])
        out = gate.evaluate_task_in_corpus(task, self._tmp.name)
        self.assertFalse(out["passed"])
        self.assertIn(gate.Reason.MANIFEST_ABSENT, out["diagnostic"])

    def test_malformed_anchor_fails_closed(self):
        # The module header promises fail-closed on EVERY abnormal input, but a
        # malformed anchor used to raise KeyError/IndexError/TypeError straight
        # out of evaluate_task -- an exception, not a deterministic verdict. Each
        # shape below must resolve to ANCHOR_MALFORMED.
        c = self._log_corpus()
        good = _anchor("pkg/log.ts", [2], ["Log"])
        malformed = {
            "missing_path": {k: v for k, v in good.items() if k != "path"},
            "missing_lines": {k: v for k, v in good.items() if k != "lines"},
            "empty_lines": dict(good, lines=[]),
            "non_integer_lines": dict(good, lines=["2"]),
            "non_list_lines": dict(good, lines=2),
            "non_string_path": dict(good, path=7),
            "not_a_mapping": "pkg/log.ts:2",
            "non_list_identifiers": dict(good, target_identifiers="Log"),
        }
        for label, anchor in sorted(malformed.items()):
            with self.subTest(shape=label):
                out = gate.evaluate_task(
                    _task("ts-central-log-namespace", [anchor]),
                    c.manifest, c.tree_dir,
                )
                self.assertFalse(out["passed"])
                self.assertEqual(self.reasons(out), [gate.Reason.ANCHOR_MALFORMED])
                # still byte-stable, and still schema-shaped
                self.assertEqual(
                    gate.to_json(out),
                    gate.to_json(
                        gate.evaluate_task(
                            _task("ts-central-log-namespace", [anchor]),
                            c.manifest, c.tree_dir,
                        )
                    ),
                )

    def test_malformed_anchor_does_not_hide_a_conforming_sibling(self):
        # A malformed anchor fails the TASK, but the well-formed sibling is still
        # evaluated on its own merits -- no short-circuit that loses evidence.
        c = self._log_corpus()
        out = gate.evaluate_task(
            _task("ts-central-log-namespace", [
                _anchor("pkg/log.ts", [2], ["Log"]),
                {"claim": "no path, no lines"},
            ]),
            c.manifest, c.tree_dir,
        )
        self.assertFalse(out["passed"])
        self.assertEqual(
            sorted(self.reasons(out)),
            sorted([gate.Reason.ANCHOR_MALFORMED, gate.Reason.CONFORMS]),
        )

    def test_tool_failure_fails_closed(self):
        c = self._log_corpus()
        rule_id = "ts-central-log-namespace"
        original = gate._RULE_HANDLERS[rule_id]

        def _boom(_segment):
            raise RuntimeError("handler exploded")

        gate._RULE_HANDLERS[rule_id] = (original[0], _boom)
        try:
            out = gate.evaluate_task(
                _task(rule_id, [_anchor("pkg/log.ts", [2], ["Log"])]),
                c.manifest, c.tree_dir,
            )
        finally:
            gate._RULE_HANDLERS[rule_id] = original
        self.assertEqual(self.reasons(out), [gate.Reason.TOOL_FAILURE])


# ---------------------------------------------------------------------------
# oracle independence (bench-harness-oracle-independence rule)
# ---------------------------------------------------------------------------


class OracleIndependenceTest(GateTestBase):
    def test_gate_ignores_sabotaged_conforms_pattern(self):
        # The authored conforms_pattern is sabotaged to a regex that MATCHES the
        # broken (sentinel-return) form. If the gate re-ran that regex, the
        # off-pattern site would spuriously PASS. Because the gate matches
        # structurally instead, it still fails the broken site and passes the
        # conforming one -- the two mechanisms are independent.
        corpus = self.corpus({"pkg/analysis.ts": TS_THROW})
        sabotaged = "return"

        broken = gate.evaluate_task(
            _task("ts-throw-typed-error",
                  [_anchor("pkg/analysis.ts", [8, 9], ["legacyExtract"])],
                  conforms_pattern=sabotaged),
            corpus.manifest, corpus.tree_dir,
        )
        self.assertFalse(broken["passed"])
        self.assertEqual(self.reasons(broken), [gate.Reason.MATCH_ABSENT])

        conforming = gate.evaluate_task(
            _task("ts-throw-typed-error",
                  [_anchor("pkg/analysis.ts", [5], ["ParseError"])],
                  conforms_pattern=sabotaged),
            corpus.manifest, corpus.tree_dir,
        )
        self.assertTrue(conforming["passed"])


# ---------------------------------------------------------------------------
# THE CONVENTION VERDICT: the AGENT's changed regions (not the bank's exemplars)
# ---------------------------------------------------------------------------


class AgentPatchTest(GateTestBase):
    """The convention gate's verdict must be about what the AGENT wrote.

    evaluate_task only re-verifies the task's own pre-authored exemplar anchors
    against the tree. Those anchors are bank material: they conform before the
    agent starts and still conform after, no matter what the agent did. An agent
    that edited somewhere else entirely -- or wrote an unrelated marker file --
    therefore scored a green convention gate for free, while `convention` is one
    third of the headline all-three-gates metric.
    """

    def _corpus(self):
        return self.corpus({"core/base.go": GO_CTOR})

    def _task(self):
        return _task(
            "go-constructor-new-pointer",
            [_anchor("core/base.go", [5], ["NewStore"])],
        )

    def test_unrelated_marker_file_no_longer_passes_convention(self):
        # The pass-agent shape: the agent writes a file the rule does not govern
        # and touches no governed construct at all. The convention claim is
        # UNPROVEN, and unproven fails closed -- it is not a pass.
        c = self._corpus()
        out = gate.evaluate_run(
            self._task(), c.manifest, c.tree_dir, {"GREEN": "ok\n"}
        )
        self.assertFalse(out["passed"])
        self.assertEqual(out["agent_patch"]["reason"], gate.Reason.PATCH_UNGOVERNED)
        # ... even though the bank's own exemplars are perfectly healthy.
        self.assertTrue(out["bank_health"]["passed"], out["bank_health"]["diagnostic"])

    def test_conforming_agent_edit_passes(self):
        c = self._corpus()
        out = gate.evaluate_run(
            self._task(), c.manifest, c.tree_dir,
            {"core/base.go": GO_CTOR.replace(
                "func BuildCache() Cache {\n\treturn Cache{}\n}",
                "func NewCache() *Cache {\n\treturn &Cache{}\n}",
            )},
        )
        self.assertTrue(out["passed"], msg=out["diagnostic"])
        self.assertEqual(out["agent_patch"]["reason"], gate.Reason.PATCH_CONFORMS)
        self.assertTrue(
            any("NewCache" in r["evidence"] for r in out["agent_patch"]["regions"]),
            out["agent_patch"]["regions"],
        )

    def test_non_conforming_agent_edit_fails_with_a_stable_reason(self):
        # The agent adds a New* constructor that returns a VALUE, not a pointer.
        c = self._corpus()
        out = gate.evaluate_run(
            self._task(), c.manifest, c.tree_dir,
            {"core/base.go": GO_CTOR + "\nfunc NewThing() Thing {\n\treturn Thing{}\n}\n"},
        )
        self.assertFalse(out["passed"])
        self.assertEqual(
            out["agent_patch"]["reason"], gate.Reason.PATCH_NONCONFORMING
        )
        self.assertIn("NewThing", out["diagnostic"])

    def test_healthy_bank_cannot_rescue_a_non_conforming_patch(self):
        # The discrimination that matters: the ONLY difference between the two
        # runs below is the agent's patch. If the verdict tracked the exemplars
        # (as it used to) both would pass.
        c = self._corpus()
        good = gate.evaluate_run(
            self._task(), c.manifest, c.tree_dir,
            {"core/new.go": "package core\n\nfunc NewThing() *Thing {\n\treturn &Thing{}\n}\n"},
        )
        bad = gate.evaluate_run(
            self._task(), c.manifest, c.tree_dir,
            {"core/new.go": "package core\n\nfunc NewThing() Thing {\n\treturn Thing{}\n}\n"},
        )
        self.assertTrue(good["passed"], msg=good["diagnostic"])
        self.assertFalse(bad["passed"])
        # both saw the same, healthy bank
        self.assertTrue(good["bank_health"]["passed"])
        self.assertTrue(bad["bank_health"]["passed"])

    def test_defective_bank_fails_the_verdict_even_on_a_conforming_patch(self):
        # Bank health is kept as its OWN field rather than merged away, but a
        # broken bank still fails the gate: a convention verdict measured with
        # defective material is not a verdict. Its reason code stays an
        # ANCHOR_* code so the scorer charges it to us, not to the agent.
        c = self._corpus()
        task = _task(
            "go-constructor-new-pointer",
            [_anchor("core/base.go", [9], ["BuildCache"])],   # not a conforming site
        )
        out = gate.evaluate_run(
            task, c.manifest, c.tree_dir,
            {"core/new.go": "package core\n\nfunc NewThing() *Thing {\n\treturn &Thing{}\n}\n"},
        )
        self.assertTrue(out["agent_patch"]["passed"])
        self.assertFalse(out["bank_health"]["passed"])
        self.assertFalse(out["passed"])

    def test_absent_patch_fails_closed(self):
        c = self._corpus()
        for patch in (None, {}):
            with self.subTest(patch=patch):
                out = gate.evaluate_run(
                    self._task(), c.manifest, c.tree_dir, patch
                )
                self.assertFalse(out["passed"])
                self.assertEqual(
                    out["agent_patch"]["reason"], gate.Reason.PATCH_ABSENT
                )

    def test_deletion_only_patch_is_ungoverned_not_conforming(self):
        c = self._corpus()
        out = gate.evaluate_run(
            self._task(), c.manifest, c.tree_dir, {"core/base.go": None}
        )
        self.assertFalse(out["passed"])
        self.assertEqual(out["agent_patch"]["reason"], gate.Reason.PATCH_UNGOVERNED)

    def test_agent_patch_ignores_the_sabotaged_conforms_pattern(self):
        # Oracle independence holds on the NEW path too: the authored regex is
        # sabotaged to accept the value-returning constructor, and the gate still
        # rejects it because it judges structurally.
        c = self._corpus()
        task = _task(
            "go-constructor-new-pointer",
            [_anchor("core/base.go", [5], ["NewStore"])],
            conforms_pattern=r"^func New\w+\(",   # matches the BROKEN form too
        )
        out = gate.evaluate_run(
            task, c.manifest, c.tree_dir,
            {"core/new.go": "package core\n\nfunc NewThing() Thing {\n\treturn Thing{}\n}\n"},
        )
        self.assertFalse(out["passed"])
        self.assertEqual(
            out["agent_patch"]["reason"], gate.Reason.PATCH_NONCONFORMING
        )

    def test_verdict_validates_against_the_published_schema(self):
        import jsonschema
        with open(
            os.path.join(CONFORMANCE, "conformance-outcome.schema.json"),
            encoding="utf-8",
        ) as handle:
            schema = json.load(handle)
        validator = jsonschema.Draft202012Validator(
            {"$ref": "#/$defs/run_outcome", "$defs": schema["$defs"]}
        )
        c = self._corpus()
        for label, patch in (
            ("conforming", {"core/new.go": "package core\n\nfunc NewThing() *Thing {\n\treturn &Thing{}\n}\n"}),
            ("broken", {"core/new.go": "package core\n\nfunc NewThing() Thing {\n\treturn Thing{}\n}\n"}),
            ("ungoverned", {"GREEN": "ok\n"}),
            ("absent", {}),
        ):
            with self.subTest(patch=label):
                validator.validate(
                    gate.evaluate_run(self._task(), c.manifest, c.tree_dir, patch)
                )

    def test_verdict_is_byte_stable_and_versioned(self):
        c = self._corpus()
        patch = {"core/new.go": "package core\n\nfunc NewThing() *Thing {\n\treturn &Thing{}\n}\n"}
        first = gate.evaluate_run(self._task(), c.manifest, c.tree_dir, patch)
        second = gate.evaluate_run(self._task(), c.manifest, c.tree_dir, patch)
        self.assertEqual(gate.to_json(first), gate.to_json(second))
        self.assertEqual(first["schema"], gate.RUN_OUTCOME_SCHEMA)
        self.assertEqual(first["bank_health"]["schema"], gate.OUTCOME_SCHEMA)
        regions = [tuple(r["lines"]) for r in first["agent_patch"]["regions"]]
        self.assertEqual(regions, sorted(regions))


class AgentPatchPerRuleTest(GateTestBase):
    """Every supported rule family discriminates on the AGENT's patch: a
    conforming edit passes and the matching broken edit fails."""

    CASES = {
        "ts-throw-typed-error": (
            "pkg/analysis.ts", TS_THROW,
            "export function f(n) {\n  throw new ParseError('x')\n}\n",
            "export function f(n) {\n  throw 'x'\n}\n",
        ),
        "ts-central-log-namespace": (
            "pkg/log.ts", TS_LOG,
            "export function f(m) {\n  Log.warn(m)\n}\n",
            "export function f(m) {\n  console.warn(m)\n}\n",
        ),
        "go-constructor-new-pointer": (
            "core/base.go", GO_CTOR,
            "package core\n\nfunc NewThing() *Thing {\n\treturn &Thing{}\n}\n",
            "package core\n\nfunc NewThing() Thing {\n\treturn Thing{}\n}\n",
        ),
        "go-request-event-handler-shape": (
            "apis/route.go", GO_HANDLER,
            "package apis\n\nfunc handleNew(e *core.RequestEvent) error {\n"
            "\treturn e.JSON(200, nil)\n}\n",
            "package apis\n\nfunc handleNew(w http.ResponseWriter, r *http.Request) {\n"
            "\tw.WriteHeader(200)\n}\n",
        ),
    }

    def test_each_rule_discriminates_on_the_agent_patch(self):
        for rule_id, (rel, body, good, bad) in sorted(self.CASES.items()):
            with self.subTest(rule=rule_id):
                c = self.corpus({rel: body})
                task = _task(rule_id, [_anchor(rel, [1], [])])
                new_rel = os.path.join(os.path.dirname(rel), "added" + os.path.splitext(rel)[1])
                ok = gate.evaluate_run(task, c.manifest, c.tree_dir, {new_rel: good})
                self.assertEqual(
                    ok["agent_patch"]["reason"], gate.Reason.PATCH_CONFORMS,
                    msg=ok["agent_patch"]["diagnostic"],
                )
                broken = gate.evaluate_run(task, c.manifest, c.tree_dir, {new_rel: bad})
                self.assertFalse(broken["agent_patch"]["passed"])
                self.assertIn(
                    broken["agent_patch"]["reason"],
                    (gate.Reason.PATCH_NONCONFORMING, gate.Reason.PATCH_UNGOVERNED),
                )

    def test_files_outside_the_rule_language_are_not_governed(self):
        # The Go constructor rule has nothing to say about a README; a changed
        # region there is neither a pass nor a fail, it is simply not evidence.
        c = self.corpus({"core/base.go": GO_CTOR})
        task = _task(
            "go-constructor-new-pointer", [_anchor("core/base.go", [5], ["NewStore"])]
        )
        out = gate.evaluate_run(
            task, c.manifest, c.tree_dir,
            {"README.md": "docs\n",
             "core/new.go": "package core\n\nfunc NewThing() *Thing {\n\treturn &Thing{}\n}\n"},
        )
        self.assertTrue(out["passed"], msg=out["diagnostic"])
        self.assertEqual(
            [r["path"] for r in out["agent_patch"]["regions"]], ["core/new.go"]
        )


# ---------------------------------------------------------------------------
# determinism (criterion 4)
# ---------------------------------------------------------------------------


class DeterminismTest(GateTestBase):
    def test_outcome_is_byte_stable_and_anchor_sorted(self):
        corpus = self.corpus({"pkg/log.ts": TS_LOG})
        # anchors supplied out of order; gate must sort by (path, lines).
        task = _task("ts-central-log-namespace", [
            _anchor("pkg/log.ts", [2], ["Log"]),
            _anchor("pkg/log.ts", [10, 11], ["Log"]),
        ])
        first = gate.evaluate_task(task, corpus.manifest, corpus.tree_dir)
        second = gate.evaluate_task(task, corpus.manifest, corpus.tree_dir)
        self.assertEqual(gate.to_json(first), gate.to_json(second))
        emitted = [tuple(a["lines"]) for a in first["anchors"]]
        self.assertEqual(emitted, sorted(emitted))
        self.assertEqual(first["schema"], gate.OUTCOME_SCHEMA)


# ---------------------------------------------------------------------------
# real committed bank: the gate runs over every task without crashing and is
# deterministic (the never-vendored corpus is absent -> fail closed).
# ---------------------------------------------------------------------------


class RealBankGateTest(unittest.TestCase):
    def test_gate_over_committed_bank_is_deterministic_and_fail_closed(self):
        tasks_dir = os.path.join(
            os.path.dirname(CONFORMANCE), "tasks"
        )
        task_files = sorted(
            n for n in os.listdir(tasks_dir) if n.endswith(".json")
        )
        self.assertGreaterEqual(len(task_files), 20)
        with tempfile.TemporaryDirectory() as empty_root:
            for name in task_files:
                with open(os.path.join(tasks_dir, name), encoding="utf-8") as fh:
                    task = json.load(fh)
                first = gate.evaluate_task_in_corpus(task, empty_root)
                second = gate.evaluate_task_in_corpus(task, empty_root)
                self.assertEqual(gate.to_json(first), gate.to_json(second))
                self.assertEqual(first["schema"], gate.OUTCOME_SCHEMA)
                # forged corpus never vendored -> fail closed, never a silent pass.
                self.assertFalse(first["passed"])
                self.assertIn(gate.Reason.MANIFEST_ABSENT, first["diagnostic"])


if __name__ == "__main__":
    unittest.main()
