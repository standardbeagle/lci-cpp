"""Tests for the gopls ground-truth oracle.

The oracle answers "where is this symbol defined, referenced, called from, and
implemented" for a pinned Go corpus, using gopls as the sole authority. It is
deliberately independent of the tool under benchmark: nothing here consults the
system under test, and a disagreement between gopls and that tool is a *result*,
never something to reconcile away.

Two classes of test live here:

  * Hermetic parse/cache/fail-fast tests. These inject a fake gopls runner and
    assert against output text captured verbatim from the real gopls v0.23.0
    binary (see CAPTURED_* below). Pinning to captured emissions rather than to
    what the docs imply is a standing rule in this repo.
  * Discrimination tests. These run the REAL gopls against the real PocketBase
    corpus, scoped to one small package to stay affordable. Mocking the oracle
    out of its own discrimination test would destroy the test's entire point,
    so they are skipped only when the pinned corpus/binary is absent.
"""

import json
import os
import shutil
import sys
import unittest

SCRIPTS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"
)
sys.path.insert(0, SCRIPTS)

import gopls_oracle  # noqa: E402


PINNED_GOPLS_VERSION = "golang.org/x/tools/gopls v0.23.0"
PINNED_CORPUS_COMMIT = "d438c6a96a0252ff9df62c1cfe193480ed9ff15d"
CORPUS_ROOT = "/home/beagle/work/core/lci-cpp/real_projects/go/pocketbase"

# --- Output captured verbatim from gopls v0.23.0 against the pinned corpus. ---
# Absolute paths are exactly what gopls emits; the oracle must relativize them,
# because absolute paths baked into goldens have burned this repo before.
_ABS = CORPUS_ROOT

CAPTURED_DEFINITION_JSON = json.dumps(
    {
        "span": {
            "uri": "file://" + _ABS + "/tools/inflector/inflector.go",
            "start": {"line": 13, "column": 6, "offset": 260},
            "end": {"line": 13, "column": 13, "offset": 267},
        },
        "description": (
            "func UcFirst(str string) string\n"
            "UcFirst converts the first character of a string into uppercase."
        ),
    }
)

CAPTURED_REFERENCES = "\n".join(
    [
        _ABS + "/plugins/jsvm/binds.go:1240:21-28",
        _ABS + "/tools/inflector/inflector.go:13:6-13",
        _ABS + "/tools/inflector/inflector.go:35:8-15",
        _ABS + "/tools/inflector/inflector_test.go:24:24-31",
    ]
)

CAPTURED_CALL_HIERARCHY = "\n".join(
    [
        "caller[0]: ranges 1240:21-28 in "
        + _ABS
        + "/plugins/jsvm/binds.go from/to function newDynamicModel in "
        + _ABS
        + "/plugins/jsvm/binds.go:1194:6-21",
        "caller[1]: ranges 35:8-15 in "
        + _ABS
        + "/tools/inflector/inflector.go from/to function Sentenize in "
        + _ABS
        + "/tools/inflector/inflector.go:29:6-15",
        "caller[2]: ranges 24:24-31 in "
        + _ABS
        + "/tools/inflector/inflector_test.go from/to function TestUcFirst in "
        + _ABS
        + "/tools/inflector/inflector_test.go:10:6-17",
        "identifier: function UcFirst in "
        + _ABS
        + "/tools/inflector/inflector.go:13:6-13",
        "callee[0]: ranges 20:24-31 in "
        + _ABS
        + "/tools/inflector/inflector.go from/to function ToUpper in "
        + "/home/beagle/go/pkg/mod/golang.org/toolchain@v0.0.1-go1.25.0."
        + "linux-amd64/src/unicode/letter.go:268:6-13",
    ]
)


class FakeGopls:
    """A scripted stand-in for the gopls CLI.

    Used only for parse/cache/fail-fast logic. It records every invocation so a
    test can assert that a cache hit performed zero subprocess work.
    """

    def __init__(self, responses, version=PINNED_GOPLS_VERSION):
        self.responses = dict(responses)
        self.version = version
        self.calls = []

    def __call__(self, args, cwd):
        self.calls.append(tuple(args))
        key = args[0]
        if key == "version":
            return self.version
        if key not in self.responses:
            raise gopls_oracle.GoplsError(
                gopls_oracle.REASON_EXEC_FAILED,
                "fake gopls has no scripted response for %r" % (args,),
            )
        value = self.responses[key]
        if isinstance(value, Exception):
            raise value
        return value


def make_oracle(tmpdir, fake):
    return gopls_oracle.GoplsOracle(
        corpus_root=CORPUS_ROOT,
        corpus_commit=PINNED_CORPUS_COMMIT,
        cache_dir=tmpdir,
        expected_version=PINNED_GOPLS_VERSION,
        runner=fake,
    )


UCFIRST = gopls_oracle.SymbolAnchor(
    name="UcFirst", path="tools/inflector/inflector.go", line=13, column=6
)


class ParsingTest(unittest.TestCase):
    """The oracle must read what gopls actually prints, and normalize it."""

    def setUp(self):
        self.tmp = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "_tmp_parse"
        )
        shutil.rmtree(self.tmp, ignore_errors=True)
        self.fake = FakeGopls(
            {
                "definition": CAPTURED_DEFINITION_JSON,
                "references": CAPTURED_REFERENCES,
                "call_hierarchy": CAPTURED_CALL_HIERARCHY,
                "implementation": "",
            }
        )
        self.oracle = make_oracle(self.tmp, self.fake)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_paths_are_relative_to_corpus_root(self):
        key = self.oracle.answer_key(UCFIRST, max_depth=1)
        self.assertEqual(key["definition"]["path"], "tools/inflector/inflector.go")
        blob = json.dumps(key)
        self.assertNotIn(CORPUS_ROOT, blob)
        self.assertNotIn("file://", blob)

    def test_references_classified_by_test_file_suffix(self):
        key = self.oracle.answer_key(UCFIRST, max_depth=1)
        kinds = {(r["path"], r["line"]): r["kind"] for r in key["references"]}
        self.assertEqual(kinds[("tools/inflector/inflector_test.go", 24)], "test")
        self.assertEqual(kinds[("tools/inflector/inflector.go", 35)], "production")
        self.assertEqual(kinds[("plugins/jsvm/binds.go", 1240)], "production")

    def test_a_path_merely_containing_test_is_not_a_test_file(self):
        # Only the _test.go suffix marks a test. A production file living under
        # a directory named e.g. "testdata" or "contest" must not be miscounted,
        # or every classification metric downstream silently skews.
        self.assertFalse(gopls_oracle.is_test_path("internal/testing/harness.go"))
        self.assertFalse(gopls_oracle.is_test_path("tools/contest/rank.go"))
        self.assertTrue(gopls_oracle.is_test_path("tools/inflector/inflector_test.go"))

    def test_lists_are_sorted_regardless_of_gopls_emission_order(self):
        shuffled = "\n".join(reversed(CAPTURED_REFERENCES.split("\n")))
        fake = FakeGopls(
            {
                "definition": CAPTURED_DEFINITION_JSON,
                "references": shuffled,
                "call_hierarchy": CAPTURED_CALL_HIERARCHY,
                "implementation": "",
            }
        )
        oracle = make_oracle(self.tmp + "b", fake)
        try:
            key = oracle.answer_key(UCFIRST, max_depth=1)
            paths = [(r["path"], r["line"]) for r in key["references"]]
            self.assertEqual(paths, sorted(paths))
        finally:
            shutil.rmtree(self.tmp + "b", ignore_errors=True)

    def test_callers_exclude_callees_and_the_identifier_line(self):
        # call_hierarchy interleaves caller/identifier/callee lines. Only the
        # caller[N] lines are incoming calls; misreading a callee as a caller
        # would invert the edge direction.
        key = self.oracle.answer_key(UCFIRST, max_depth=1)
        names = {c["name"] for c in key["callers"]}
        self.assertEqual(names, {"newDynamicModel", "Sentenize", "TestUcFirst"})
        self.assertNotIn("ToUpper", names)


class FailFastTest(unittest.TestCase):
    """A silent oracle grades every answer as a miss. It must scream instead."""

    def setUp(self):
        self.tmp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_tmp_ff")
        shutil.rmtree(self.tmp, ignore_errors=True)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_missing_gopls_binary_raises_with_reason(self):
        with self.assertRaises(gopls_oracle.GoplsError) as ctx:
            gopls_oracle.resolve_gopls_binary(
                candidate="/nonexistent/path/to/gopls",
            )
        self.assertEqual(ctx.exception.reason, gopls_oracle.REASON_GOPLS_MISSING)

    def test_version_mismatch_raises_with_reason(self):
        fake = FakeGopls({}, version="golang.org/x/tools/gopls v0.99.0")
        oracle = make_oracle(self.tmp, fake)
        with self.assertRaises(gopls_oracle.GoplsError) as ctx:
            oracle.verify_version()
        self.assertEqual(ctx.exception.reason, gopls_oracle.REASON_VERSION_MISMATCH)
        self.assertIn("v0.99.0", str(ctx.exception))
        self.assertIn("v0.23.0", str(ctx.exception))

    def test_gopls_error_propagates_and_is_never_swallowed(self):
        fake = FakeGopls(
            {
                "definition": gopls_oracle.GoplsError(
                    gopls_oracle.REASON_EXEC_FAILED, "gopls: identifier not found"
                )
            }
        )
        oracle = make_oracle(self.tmp, fake)
        with self.assertRaises(gopls_oracle.GoplsError) as ctx:
            oracle.answer_key(UCFIRST, max_depth=1)
        self.assertEqual(ctx.exception.reason, gopls_oracle.REASON_EXEC_FAILED)

    def test_empty_reference_set_for_a_real_symbol_raises(self):
        # `references -d` includes the declaration, so a resolvable symbol has
        # at least one reference. Zero means gopls failed to load the package
        # while still exiting 0 -- exactly the "grade everything a miss" trap.
        fake = FakeGopls(
            {
                "definition": CAPTURED_DEFINITION_JSON,
                "references": "",
                "call_hierarchy": CAPTURED_CALL_HIERARCHY,
                "implementation": "",
            }
        )
        oracle = make_oracle(self.tmp, fake)
        with self.assertRaises(gopls_oracle.GoplsError) as ctx:
            oracle.answer_key(UCFIRST, max_depth=1)
        self.assertEqual(ctx.exception.reason, gopls_oracle.REASON_EMPTY_RESULT)

    def test_no_empty_key_is_ever_written_to_cache_on_failure(self):
        fake = FakeGopls(
            {
                "definition": CAPTURED_DEFINITION_JSON,
                "references": "",
                "call_hierarchy": CAPTURED_CALL_HIERARCHY,
                "implementation": "",
            }
        )
        oracle = make_oracle(self.tmp, fake)
        with self.assertRaises(gopls_oracle.GoplsError):
            oracle.answer_key(UCFIRST, max_depth=1)
        written = []
        for root, _dirs, files in os.walk(self.tmp):
            written.extend(os.path.join(root, f) for f in files)
        self.assertEqual(written, [])


class TransitiveCallerTest(unittest.TestCase):
    """Depth>=2 incoming calls, with the cycle guard Go recursion demands."""

    def setUp(self):
        self.tmp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_tmp_tc")
        shutil.rmtree(self.tmp, ignore_errors=True)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _hierarchy(self, callers):
        lines = []
        for i, (name, path, line) in enumerate(callers):
            lines.append(
                "caller[%d]: ranges %d:1-2 in %s/%s from/to function %s in %s/%s:%d:6-9"
                % (i, line, _ABS, path, name, _ABS, path, line)
            )
        return "\n".join(lines)

    def test_callers_are_collected_to_depth_two(self):
        # a <- b <- c : querying a at depth 2 must surface c, not just b.
        graph = {
            (13, 6): self._hierarchy([("b", "pkg/b.go", 20)]),
            (20, 6): self._hierarchy([("c", "pkg/c.go", 30)]),
            (30, 6): "",
        }
        oracle = gopls_oracle.GoplsOracle(
            corpus_root=CORPUS_ROOT,
            corpus_commit=PINNED_CORPUS_COMMIT,
            cache_dir=self.tmp,
            expected_version=PINNED_GOPLS_VERSION,
            runner=_PositionalFake(graph),
        )
        key = oracle.answer_key(UCFIRST, max_depth=2)
        by_name = {c["name"]: c["depth"] for c in key["callers"]}
        self.assertEqual(by_name, {"b": 1, "c": 2})

    def test_caller_cycle_terminates(self):
        # a <- b <- a. Without a visited-set this recursion never returns.
        graph = {
            (13, 6): self._hierarchy([("b", "pkg/b.go", 20)]),
            (20, 6): self._hierarchy([("UcFirst", "tools/inflector/inflector.go", 13)]),
        }
        oracle = gopls_oracle.GoplsOracle(
            corpus_root=CORPUS_ROOT,
            corpus_commit=PINNED_CORPUS_COMMIT,
            cache_dir=self.tmp,
            expected_version=PINNED_GOPLS_VERSION,
            runner=_PositionalFake(graph),
        )
        key = oracle.answer_key(UCFIRST, max_depth=5)
        self.assertIn("b", {c["name"] for c in key["callers"]})

    def test_depth_is_capped(self):
        # A chain longer than max_depth must stop at max_depth exactly.
        graph = {
            (13, 6): self._hierarchy([("b", "pkg/b.go", 20)]),
            (20, 6): self._hierarchy([("c", "pkg/c.go", 30)]),
            (30, 6): self._hierarchy([("d", "pkg/d.go", 40)]),
            (40, 6): "",
        }
        oracle = gopls_oracle.GoplsOracle(
            corpus_root=CORPUS_ROOT,
            corpus_commit=PINNED_CORPUS_COMMIT,
            cache_dir=self.tmp,
            expected_version=PINNED_GOPLS_VERSION,
            runner=_PositionalFake(graph),
        )
        key = oracle.answer_key(UCFIRST, max_depth=2)
        self.assertEqual({c["name"] for c in key["callers"]}, {"b", "c"})


class _PositionalFake(FakeGopls):
    """Fake whose call_hierarchy answer depends on the queried position."""

    def __init__(self, graph):
        super().__init__({})
        self.graph = graph

    def __call__(self, args, cwd):
        self.calls.append(tuple(args))
        if args[0] == "version":
            return self.version
        if args[0] == "definition":
            return CAPTURED_DEFINITION_JSON
        if args[0] == "references":
            return CAPTURED_REFERENCES
        if args[0] == "implementation":
            return ""
        if args[0] == "call_hierarchy":
            pos = args[-1]
            _, line, col = pos.rsplit(":", 2)
            return self.graph.get((int(line), int(col)), "")
        raise AssertionError("unexpected %r" % (args,))


class CacheTest(unittest.TestCase):
    """Cold gopls over 150k LOC is brutal; the cache must be real and correct."""

    def setUp(self):
        self.tmp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_tmp_c")
        shutil.rmtree(self.tmp, ignore_errors=True)
        self.responses = {
            "definition": CAPTURED_DEFINITION_JSON,
            "references": CAPTURED_REFERENCES,
            "call_hierarchy": CAPTURED_CALL_HIERARCHY,
            "implementation": "",
        }

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_second_lookup_hits_cache_and_runs_no_gopls(self):
        fake = FakeGopls(self.responses)
        oracle = make_oracle(self.tmp, fake)
        first = oracle.answer_key(UCFIRST, max_depth=1)
        calls_after_first = len(fake.calls)
        self.assertGreater(calls_after_first, 0)

        fake2 = FakeGopls(self.responses)
        oracle2 = make_oracle(self.tmp, fake2)
        second = oracle2.answer_key(UCFIRST, max_depth=1)
        self.assertEqual(fake2.calls, [], "cache hit must not invoke gopls")
        self.assertEqual(first, second)

    def test_new_corpus_commit_invalidates_cache(self):
        fake = FakeGopls(self.responses)
        make_oracle(self.tmp, fake).answer_key(UCFIRST, max_depth=1)

        fake2 = FakeGopls(self.responses)
        moved = gopls_oracle.GoplsOracle(
            corpus_root=CORPUS_ROOT,
            corpus_commit="0" * 40,
            cache_dir=self.tmp,
            expected_version=PINNED_GOPLS_VERSION,
            runner=fake2,
        )
        moved.answer_key(UCFIRST, max_depth=1)
        self.assertGreater(len(fake2.calls), 0, "commit change must miss cache")

    def test_new_gopls_version_invalidates_cache(self):
        fake = FakeGopls(self.responses)
        make_oracle(self.tmp, fake).answer_key(UCFIRST, max_depth=1)

        fake2 = FakeGopls(self.responses, version="golang.org/x/tools/gopls v0.24.0")
        upgraded = gopls_oracle.GoplsOracle(
            corpus_root=CORPUS_ROOT,
            corpus_commit=PINNED_CORPUS_COMMIT,
            cache_dir=self.tmp,
            expected_version="golang.org/x/tools/gopls v0.24.0",
            runner=fake2,
        )
        upgraded.answer_key(UCFIRST, max_depth=1)
        self.assertGreater(len(fake2.calls), 0, "version change must miss cache")

    def test_cached_key_records_its_provenance(self):
        fake = FakeGopls(self.responses)
        key = make_oracle(self.tmp, fake).answer_key(UCFIRST, max_depth=1)
        self.assertEqual(key["corpus_commit"], PINNED_CORPUS_COMMIT)
        self.assertEqual(key["gopls_version"], PINNED_GOPLS_VERSION)

    def test_cache_entry_from_a_foreign_commit_is_rejected_not_trusted(self):
        # Defence in depth: even if a stale file is reachable at the cache path,
        # a provenance mismatch inside it must fail loud rather than be served.
        fake = FakeGopls(self.responses)
        oracle = make_oracle(self.tmp, fake)
        oracle.answer_key(UCFIRST, max_depth=1)
        path = oracle.cache_path(UCFIRST, max_depth=1)
        with open(path) as fh:
            payload = json.load(fh)
        payload["corpus_commit"] = "f" * 40
        with open(path, "w") as fh:
            json.dump(payload, fh)
        with self.assertRaises(gopls_oracle.GoplsError) as ctx:
            make_oracle(self.tmp, FakeGopls(self.responses)).answer_key(
                UCFIRST, max_depth=1
            )
        self.assertEqual(ctx.exception.reason, gopls_oracle.REASON_CACHE_PROVENANCE)


def _real_gopls_available():
    try:
        binary = gopls_oracle.resolve_gopls_binary()
    except gopls_oracle.GoplsError:
        return False
    return os.path.isdir(CORPUS_ROOT) and bool(binary)


REAL = unittest.skipUnless(
    _real_gopls_available(), "pinned gopls binary and PocketBase corpus required"
)


@REAL
class DiscriminationTest(unittest.TestCase):
    """Prove the oracle is an oracle.

    A validator that only demonstrates "the right answer passes" has not been
    shown to discriminate at all. For every claim the oracle makes, we also
    assert that a deliberately wrong claim FAILS. Ground truth here was
    established by hand, by reading the PocketBase source at the pinned commit
    -- not by asking any indexing tool.

    Scoped to tools/inflector (~450 LOC, few deps) to keep real-gopls cost sane.
    """

    @classmethod
    def setUpClass(cls):
        cls.tmp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_tmp_real")
        shutil.rmtree(cls.tmp, ignore_errors=True)
        cls.oracle = gopls_oracle.GoplsOracle.for_corpus(
            corpus_root=CORPUS_ROOT, cache_dir=cls.tmp
        )
        cls.key = cls.oracle.answer_key(UCFIRST, max_depth=2)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    # Hand-verified against PocketBase @ d438c6a. `UcFirst` is called from
    # exactly three places in the tree:
    #   plugins/jsvm/binds.go:1240        inside newDynamicModel  (production)
    #   tools/inflector/inflector.go:35   inside Sentenize        (production)
    #   tools/inflector/inflector_test.go:24 inside TestUcFirst   (test)
    # The only other textual occurrence, inflector.go:12, is a doc comment and
    # is correctly NOT a reference.
    MANUAL_DIRECT_CALLERS = {"newDynamicModel", "Sentenize", "TestUcFirst"}

    def test_oracle_matches_the_hand_verified_caller_set(self):
        direct = {c["name"] for c in self.key["callers"] if c["depth"] == 1}
        self.assertEqual(direct, self.MANUAL_DIRECT_CALLERS)

    def test_oracle_rejects_a_deliberately_wrong_caller_set(self):
        # The failure case that makes the assertion above meaningful. Each
        # mutation is one a buggy oracle could plausibly produce.
        direct = {c["name"] for c in self.key["callers"] if c["depth"] == 1}

        dropped = self.MANUAL_DIRECT_CALLERS - {"Sentenize"}
        self.assertNotEqual(direct, dropped, "must not miss an in-package caller")

        invented = self.MANUAL_DIRECT_CALLERS | {"Columnify"}
        self.assertNotEqual(direct, invented, "must not invent a non-caller")

        # ToUpper is a CALLEE of UcFirst. An oracle that inverts edge direction
        # would report it here.
        inverted = self.MANUAL_DIRECT_CALLERS | {"ToUpper"}
        self.assertNotEqual(direct, inverted, "must not report callees as callers")

    def test_oracle_rejects_a_deliberately_wrong_definition(self):
        self.assertEqual(self.key["definition"]["line"], 13)
        self.assertNotEqual(
            self.key["definition"]["line"], 12, "line 12 is the doc comment, not the def"
        )

    def test_oracle_rejects_a_deliberately_wrong_reference_set(self):
        refs = {(r["path"], r["line"]) for r in self.key["references"]}
        self.assertEqual(
            refs,
            {
                ("plugins/jsvm/binds.go", 1240),
                ("tools/inflector/inflector.go", 13),
                ("tools/inflector/inflector.go", 35),
                ("tools/inflector/inflector_test.go", 24),
            },
        )
        self.assertNotIn(
            ("tools/inflector/inflector.go", 12),
            refs,
            "a doc comment mentioning the name is not a reference",
        )

    def test_oracle_classifies_the_hand_verified_test_reference(self):
        kinds = {(r["path"], r["line"]): r["kind"] for r in self.key["references"]}
        self.assertEqual(kinds[("tools/inflector/inflector_test.go", 24)], "test")
        self.assertEqual(kinds[("plugins/jsvm/binds.go", 1240)], "production")
        # And the wrong classification must be detectable.
        self.assertNotEqual(kinds[("plugins/jsvm/binds.go", 1240)], "test")

    def test_transitive_callers_reach_depth_two(self):
        # Sentenize (depth 1) is itself called by NewApiError and
        # resolveSafeErrorItem in tools/router/error.go -- hand-verified.
        depth2 = {c["name"] for c in self.key["callers"] if c["depth"] == 2}
        self.assertIn("NewApiError", depth2)
        self.assertIn("resolveSafeErrorItem", depth2)


@REAL
class ByteIdenticalTest(unittest.TestCase):
    """Same corpus commit + same gopls version => byte-identical key."""

    def test_two_cold_runs_are_byte_identical(self):
        tmp_a = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_tmp_a")
        tmp_b = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_tmp_b")
        for d in (tmp_a, tmp_b):
            shutil.rmtree(d, ignore_errors=True)
        try:
            a = gopls_oracle.GoplsOracle.for_corpus(
                corpus_root=CORPUS_ROOT, cache_dir=tmp_a
            )
            b = gopls_oracle.GoplsOracle.for_corpus(
                corpus_root=CORPUS_ROOT, cache_dir=tmp_b
            )
            key_a = a.answer_key(UCFIRST, max_depth=2)
            key_b = b.answer_key(UCFIRST, max_depth=2)
            self.assertEqual(
                gopls_oracle.serialize(key_a), gopls_oracle.serialize(key_b)
            )
        finally:
            for d in (tmp_a, tmp_b):
                shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
