"""Tests for the objective symbol-cohort selector.

The selector picks the symbols the discovery sweep runs on. Its entire reason to
exist is that the symbols must be chosen by MEASURED properties, never by
intuition -- intuition about "which symbols are hard" is the hypothesis this
epic tests, so hand-picking the sample would smuggle the conclusion into it.

Two classes of test live here, mirroring test_gopls_oracle.py:

  * Hermetic tests. A tiny Go corpus is synthesized in a tmpdir and a fake
    oracle is injected. Real grep runs against that tmpdir -- grep is cheap and
    faking it would leave the one mechanism the noise axis depends on untested.
  * Discrimination tests. These run the REAL gopls + real grep against the
    pinned PocketBase corpus and assert the selector actually separates a known
    high-collision symbol from a known unique one, in BOTH directions.

Per .claude/rules/bench-harness-oracle-independence.md, the expectations here
are derived independently of the selector: the numbers below were measured by
hand with grep(1) and gopls(1) directly, NOT by calling the code under test.
Reusing the selector's own arithmetic to check the selector would make a shared
blind spot invisible by construction.
"""

import json
import os
import shutil
import subprocess
import sys
import unittest

SCRIPTS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"
)
sys.path.insert(0, SCRIPTS)

import select_symbol_cohorts as sc  # noqa: E402


CORPUS_ROOT = "/home/beagle/work/core/lci-cpp/real_projects/go/pocketbase"
PINNED_CORPUS_COMMIT = "d438c6a96a0252ff9df62c1cfe193480ed9ff15d"
PINNED_GOPLS_VERSION = "golang.org/x/tools/gopls v0.23.0"

# --- Independently measured ground truth ------------------------------------
# Measured by hand against the pinned corpus, NOT produced by the selector:
#
#   grep -rnwc --include='*.go' Type . | awk -F: '{s+=$2} END {print s}'   -> 351
#   gopls references -d core/field_geo_point.go:64:27 | wc -l              ->  29
#   grep -rnwc --include='*.go' vacuum .                                   ->   3
#   gopls references -d core/db_table.go:133:22 | wc -l                    ->   3
#
# GeoPointField.Type collides with 13 other Type declarations -> noise ~12.1.
# BaseApp.vacuum is uniquely named -> noise 1.0, the CONTROL shape.
HIGH_COLLISION = {
    "name": "Type",
    "path": "core/field_geo_point.go",
    "line": 64,
    "column": 27,
    "grep_hits": 351,
    "true_refs": 29,
}
UNIQUE_CONTROL = {
    "name": "vacuum",
    "path": "core/db_table.go",
    "line": 133,
    "column": 22,
    "grep_hits": 3,
    "true_refs": 3,
}


def real_corpus_available():
    return (
        os.path.isdir(CORPUS_ROOT)
        and shutil.which("grep") is not None
        and sc.resolve_gopls_binary_or_none() is not None
    )


REQUIRES_CORPUS = unittest.skipUnless(
    real_corpus_available(), "pinned corpus or gopls absent"
)


def write_corpus(root, files):
    for rel, body in files.items():
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf8") as handle:
            handle.write(body)
    return root


# A miniature corpus with a deliberate collision: Type is declared on two
# receivers plus a bare type, while lonely is declared once.
MINI_CORPUS = {
    "a/alpha.go": (
        "package a\n"
        "\n"
        "type Alpha struct{}\n"
        "\n"
        "func (x *Alpha) Type() string {\n"
        '\treturn "alpha"\n'
        "}\n"
        "\n"
        "func lonely(n int) int {\n"
        "\treturn n + 1\n"
        "}\n"
    ),
    "b/beta.go": (
        "package b\n"
        "\n"
        "type Beta struct{}\n"
        "\n"
        "func (x *Beta) Type() string {\n"
        '\treturn "beta"\n'
        "}\n"
        "\n"
        "func useType(x *Beta) string {\n"
        "\treturn x.Type()\n"
        "}\n"
    ),
    "b/beta_test.go": (
        "package b\n"
        "\n"
        "type stub struct{}\n"
        "\n"
        "func (s *stub) Type() string {\n"
        '\treturn "stub"\n'
        "}\n"
        "\n"
        "func TestType(t *T) {\n"
        "\tvar x Beta\n"
        "\t_ = x.Type()\n"
        "}\n"
    ),
}


class FakeOracle:
    """Stands in for gopls. Returns whatever the test declares as truth."""

    def __init__(self, keys):
        self.keys = keys
        self.calls = []

    def answer_key(self, anchor, max_depth=1):
        self.calls.append((anchor.name, anchor.path, anchor.line, anchor.column))
        try:
            return self.keys[(anchor.path, anchor.line, anchor.column)]
        except KeyError:
            raise AssertionError("fake oracle asked for unexpected anchor %r" % (anchor,))


def fake_key(total, test=0, callers=0, impls=0):
    production = total - test
    return {
        "reference_counts": {"total": total, "test": test, "production": production},
        "callers": [{"depth": 1, "name": "c%d" % i} for i in range(callers)],
        "implementations": [{"path": "i%d.go" % i} for i in range(impls)],
    }


class EnumerationTest(unittest.TestCase):
    def setUp(self):
        import tempfile

        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root)
        write_corpus(self.root, MINI_CORPUS)

    def test_enumerates_funcs_methods_and_types_from_production_files(self):
        cands = sc.enumerate_declarations(self.root)
        found = {(c.name, c.path) for c in cands}
        self.assertIn(("Type", "a/alpha.go"), found)
        self.assertIn(("Type", "b/beta.go"), found)
        self.assertIn(("lonely", "a/alpha.go"), found)
        self.assertIn(("Alpha", "a/alpha.go"), found)

    def test_test_file_declarations_are_not_candidates(self):
        # A _test.go declaration must never become a benchmark cell: the sweep
        # measures how tools find production symbols.
        cands = sc.enumerate_declarations(self.root)
        self.assertNotIn("b/beta_test.go", {c.path for c in cands})
        self.assertNotIn("TestType", {c.name for c in cands})

    def test_declaration_counts_include_test_files(self):
        # Shadowing is a property of the identifier across the whole corpus. A
        # name also declared in tests is still shadowed, so def_count counts it
        # even though it cannot itself be selected.
        counts = sc.declaration_counts(self.root)
        # Alpha.Type + Beta.Type + stub.Type (the last one in beta_test.go).
        self.assertEqual(counts["Type"], 3)
        self.assertEqual(counts["lonely"], 1)

    def test_declaration_counts_still_count_kinds_that_cannot_be_candidates(self):
        # Being ineligible to BE a benchmark cell and shadowing a name are
        # different things. A type declaration still collides with a method of
        # the same name in grep's output, so it must keep counting here even
        # though enumerate_declarations refuses to select it.
        counts = sc.declaration_counts(self.root)
        self.assertEqual(counts["Alpha"], 1)
        self.assertEqual(counts["Beta"], 1)

    def test_type_declarations_are_not_candidates(self):
        # gopls call_hierarchy refuses a type ("X is not a function"), so a
        # type can never be measured on the fan-in axis and is dropped by the
        # oracle every single time. Enumerating types anyway does not add
        # breadth -- it silently deletes one declaration kind from the sample
        # after it has already consumed pool slots.
        names = {c.name for c in sc.enumerate_declarations(self.root)}
        self.assertNotIn("Alpha", names)
        self.assertNotIn("Beta", names)
        self.assertIn("lonely", names)

    def test_compiler_invoked_entrypoints_are_not_candidates(self):
        # init and main are called by the runtime, never referenced in source.
        # Their reference count is ~0 by construction, so their noise ratio is
        # an artifact of that rather than a measure of collision, and "find
        # references to init" is not a question any developer asks.
        write_corpus(
            self.root,
            {
                "c/gamma.go": (
                    "package c\n\nfunc init() {\n}\n\nfunc main() {\n}\n\n"
                    "func real() int {\n\treturn 1\n}\n"
                )
            },
        )
        names = {c.name for c in sc.enumerate_declarations(self.root)}
        self.assertNotIn("init", names)
        self.assertNotIn("main", names)
        self.assertIn("real", names)

    def test_enumeration_is_ordered_deterministically(self):
        first = [c.slug() for c in sc.enumerate_declarations(self.root)]
        second = [c.slug() for c in sc.enumerate_declarations(self.root)]
        self.assertEqual(first, second)
        self.assertEqual(first, sorted(first))

    def test_anchor_column_points_at_the_identifier(self):
        # gopls resolves a cursor position, not a name. An off-by-one column
        # silently resolves the receiver or the func keyword instead.
        cands = {(c.name, c.path): c for c in sc.enumerate_declarations(self.root)}
        alpha_type = cands[("Type", "a/alpha.go")]
        line = MINI_CORPUS["a/alpha.go"].split("\n")[alpha_type.line - 1]
        self.assertEqual(
            line[alpha_type.column - 1 : alpha_type.column - 1 + len("Type")], "Type"
        )


class GrepHitCountTest(unittest.TestCase):
    def setUp(self):
        import tempfile

        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root)
        write_corpus(self.root, MINI_CORPUS)

    def test_counts_word_boundary_hits_with_real_grep(self):
        # Independently: grep -rnw Type over MINI_CORPUS matches 3 decls
        # (Alpha, Beta, stub) + 2 call sites = 5 lines. The func TestType line
        # must NOT match, which is exactly what -w buys.
        self.assertEqual(sc.grep_hit_count("Type", self.root), 5)

    def test_counts_are_whole_word_only(self):
        self.assertEqual(sc.grep_hit_count("lonely", self.root), 1)
        self.assertEqual(sc.grep_hit_count("onel", self.root), 0)

    def test_grep_counts_include_test_files(self):
        # The sweep's grep baseline greps the whole corpus, so the noise axis
        # must reflect the noise a real grep user actually wades through.
        hits = sc.grep_hit_count("Type", self.root)
        no_tests = sc.grep_hit_count("Type", os.path.join(self.root, "a"))
        self.assertGreater(hits, no_tests)


class NoiseClassificationTest(unittest.TestCase):
    def test_ratio_is_grep_over_true_references(self):
        self.assertAlmostEqual(sc.noise_ratio(351, 29), 351 / 29.0)

    def test_zero_true_references_is_refused_not_defaulted(self):
        # A zero denominator means the oracle failed. Silently clamping it to 1
        # would mint an infinitely-noisy phantom cell out of a broken run.
        with self.assertRaises(sc.SelectionError):
            sc.noise_ratio(351, 0)

    def test_unique_low_noise_symbol_is_control(self):
        self.assertEqual(sc.classify_noise(1.0, def_count=1), "control")

    def test_shadowed_symbol_is_never_control_however_quiet(self):
        # Control means "grep should trivially win here". A shadowed name does
        # not qualify even if this particular receiver is quiet.
        self.assertNotEqual(sc.classify_noise(1.0, def_count=4), "control")

    def test_high_ratio_is_high(self):
        self.assertEqual(sc.classify_noise(12.1, def_count=14), "high")

    def test_mid_ratio_is_medium(self):
        self.assertEqual(sc.classify_noise(4.4, def_count=6), "medium")


class CohortAssignmentTest(unittest.TestCase):
    def rows(self):
        return [
            sc.build_metrics_row(
                sc.Candidate("vacuum", "core/db_table.go", 133, 22, "method"),
                grep_hits=3,
                def_count=1,
                key=fake_key(total=3, test=0, callers=2, impls=0),
            ),
            sc.build_metrics_row(
                sc.Candidate("Type", "core/field_geo_point.go", 64, 27, "method"),
                grep_hits=351,
                def_count=14,
                key=fake_key(total=29, test=8, callers=14, impls=1),
            ),
            sc.build_metrics_row(
                sc.Candidate("Set", "core/record_model.go", 893, 21, "method"),
                grep_hits=529,
                def_count=6,
                key=fake_key(total=119, test=94, callers=46, impls=0),
            ),
        ]

    def test_row_carries_every_metric_d5_correlates_on(self):
        row = self.rows()[1]
        for field in (
            "noise_ratio",
            "grep_hit_count",
            "true_reference_count",
            "fan_in",
            "def_count",
            "impl_count",
            "test_reference_count",
            "production_reference_count",
        ):
            self.assertIn(field, row, "D5 correlates on %s" % field)

    def test_cohorts_span_every_declared_axis(self):
        cohorts = sc.assign_cohorts(self.rows())
        for axis in (
            "noise_control",
            "noise_medium",
            "noise_high",
            "fan_in_low",
            "fan_in_high",
            "shadowing_unique",
            "shadowing_multi",
            "impl_none",
            "impl_many",
            "refs_test_dominant",
            "refs_production_dominant",
        ):
            self.assertIn(axis, cohorts)

    def test_control_symbol_lands_in_control_and_unique_cohorts(self):
        cohorts = sc.assign_cohorts(self.rows())
        self.assertIn("vacuum", [r["name"] for r in cohorts["noise_control"]])
        self.assertIn("vacuum", [r["name"] for r in cohorts["shadowing_unique"]])

    def test_test_dominant_split_uses_measured_reference_kinds(self):
        cohorts = sc.assign_cohorts(self.rows())
        # Set: 94 of 119 references are in _test.go files.
        self.assertIn("Set", [r["name"] for r in cohorts["refs_test_dominant"]])
        self.assertIn("vacuum", [r["name"] for r in cohorts["refs_production_dominant"]])


class RiggedSweepGuardTest(unittest.TestCase):
    def test_empty_control_cohort_is_refused(self):
        # A sweep with no control cells can only report that LCI wins. That is
        # a rigged experiment, and the epic's honesty constraint depends on
        # being able to report a null result -- so this fails loudly.
        noisy_only = [
            sc.build_metrics_row(
                sc.Candidate("Type", "core/field_geo_point.go", 64, 27, "method"),
                grep_hits=351,
                def_count=14,
                key=fake_key(total=29, test=8, callers=14, impls=1),
            )
        ]
        with self.assertRaises(sc.SelectionError) as ctx:
            sc.validate_cohorts(sc.assign_cohorts(noisy_only))
        self.assertIn("control", str(ctx.exception).lower())


def synthetic_corpus(root, count=80):
    """A corpus big enough that the seed has room to act.

    A handful of symbols cannot exercise seeded sampling: with fewer
    candidates than strata*quota every stratum is drawn whole and the seed is
    unobservable by construction. Sizing this above the sample size is what
    makes the seed test meaningful rather than vacuously green.
    """
    files = {}
    for index in range(count):
        pkg = "p%d" % (index % 8)
        files.setdefault("%s/%s.go" % (pkg, pkg), "package %s\n\n" % pkg)
        files["%s/%s.go" % (pkg, pkg)] += (
            "func fn%02d(n int) int {\n\treturn n + %d\n}\n\n" % (index, index)
        )
    return write_corpus(root, files)


def git_init(root):
    """corpus_commit() reads git, so a corpus fixture must be a real repo."""
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
    for args in (["init", "-q"], ["add", "-A"], ["commit", "-qm", "fixture"]):
        subprocess.run(["git"] + args, cwd=root, env=env, check=True,
                       capture_output=True)
    return root


class DeterminismTest(unittest.TestCase):
    def setUp(self):
        import tempfile

        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root)
        synthetic_corpus(self.root)
        git_init(self.root)
        self.cands = sc.enumerate_declarations(self.root)
        # Spread hits so stratification has real strata to draw from.
        self.hits = {c.name: (i % 40) + 1 for i, c in enumerate(self.cands)}
        self.keys = {
            (c.path, c.line, c.column): fake_key(total=3, test=1, callers=1)
            for c in self.cands
        }

    def test_same_seed_yields_identical_pool(self):
        first = sc.stratified_pool(self.cands, self.hits, seed="s", size=12)
        second = sc.stratified_pool(self.cands, self.hits, seed="s", size=12)
        self.assertEqual([c.slug() for c in first], [c.slug() for c in second])

    def test_pool_ordering_does_not_depend_on_input_ordering(self):
        forward = sc.stratified_pool(self.cands, self.hits, seed="s", size=12)
        backward = sc.stratified_pool(
            list(reversed(self.cands)), self.hits, seed="s", size=12
        )
        self.assertEqual([c.slug() for c in forward], [c.slug() for c in backward])

    def test_pool_is_stable_across_processes(self):
        # hash() is salted per process, so a pool built on it would differ run
        # to run while looking deterministic inside one process.
        script = (
            "import sys; sys.path.insert(0, %r);\n"
            "import select_symbol_cohorts as sc;\n"
            "cands = sc.enumerate_declarations(%r);\n"
            "hits = {c.name: (i %% 40) + 1 for i, c in enumerate(cands)};\n"
            "print(','.join(c.slug() for c in "
            "sc.stratified_pool(cands, hits, seed='s', size=12)))"
            % (SCRIPTS, self.root)
        )
        runs = set()
        for _ in range(3):
            out = subprocess.run(
                [sys.executable, "-c", script],
                capture_output=True, text=True,
                env=dict(os.environ, PYTHONHASHSEED="random"),
            )
            self.assertEqual(out.returncode, 0, out.stderr)
            runs.add(out.stdout.strip())
        self.assertEqual(len(runs), 1, "pool differs across processes: %r" % runs)

    def test_different_seed_yields_a_different_pool(self):
        # If the seed did nothing, "deterministic from a committed seed" would
        # be a false claim -- the pool would just be an arbitrary fixed prefix.
        baseline = tuple(
            c.slug() for c in sc.stratified_pool(self.cands, self.hits, seed="a", size=12)
        )
        variants = {
            tuple(
                c.slug()
                for c in sc.stratified_pool(self.cands, self.hits, seed=seed, size=12)
            )
            for seed in ("b", "c", "d")
        }
        self.assertTrue(
            any(v != baseline for v in variants), "seed has no effect on selection"
        )

    def test_full_selection_is_byte_identical_across_runs(self):
        def run():
            return sc.select(
                corpus_root=self.root,
                oracle=FakeOracle(self.keys),
                seed="fixed-seed",
                pool_size=12,
                validate=False,
            )

        self.assertEqual(sc.serialize(run()), sc.serialize(run()))


class LciIndependenceTest(unittest.TestCase):
    def test_selector_never_references_the_tool_under_test(self):
        # The oracle and the sample must both be independent of LCI, or the
        # benchmark grades LCI against itself.
        with open(os.path.join(SCRIPTS, "select_symbol_cohorts.py")) as handle:
            source = handle.read().lower()
        for forbidden in ("import lci", "from lci", "build/release/src/lci", "lci_"):
            self.assertNotIn(forbidden, source)


@REQUIRES_CORPUS
class DiscriminationTest(unittest.TestCase):
    """The selector must actually separate real symbols, both directions.

    A selector that merely "runs and produces cohorts" is unproven. These
    assert the known-hard symbol lands high and the known-easy one lands in
    control -- AND the negatives, that neither lands in the other's bucket.
    """

    @classmethod
    def setUpClass(cls):
        cls.oracle = sc.build_oracle(CORPUS_ROOT)

    def measured(self, spec):
        anchor = sc.SymbolAnchor(
            spec["name"], spec["path"], spec["line"], spec["column"]
        )
        key = self.oracle.answer_key(anchor, max_depth=1)
        return sc.build_metrics_row(
            sc.Candidate(spec["name"], spec["path"], spec["line"], spec["column"], "method"),
            grep_hits=sc.grep_hit_count(spec["name"], CORPUS_ROOT),
            def_count=sc.declaration_counts(CORPUS_ROOT)[spec["name"]],
            key=key,
        )

    def test_grep_hits_match_independently_measured_counts(self):
        # Pins the noise numerator against grep(1) run by hand. If the
        # selector's grep invocation drifts, this catches it.
        self.assertEqual(
            sc.grep_hit_count(HIGH_COLLISION["name"], CORPUS_ROOT),
            HIGH_COLLISION["grep_hits"],
        )
        self.assertEqual(
            sc.grep_hit_count(UNIQUE_CONTROL["name"], CORPUS_ROOT),
            UNIQUE_CONTROL["grep_hits"],
        )

    def test_known_high_collision_symbol_lands_in_the_high_noise_cohort(self):
        row = self.measured(HIGH_COLLISION)
        self.assertEqual(row["true_reference_count"], HIGH_COLLISION["true_refs"])
        self.assertEqual(row["noise_cohort"], "high")

    def test_known_unique_symbol_lands_in_the_control_cohort(self):
        row = self.measured(UNIQUE_CONTROL)
        self.assertEqual(row["true_reference_count"], UNIQUE_CONTROL["true_refs"])
        self.assertEqual(row["noise_cohort"], "control")

    def test_the_negative_high_collision_symbol_is_not_a_control(self):
        # The discriminating half. Without this, a selector that labels
        # everything "control" would pass the positive test above.
        self.assertNotEqual(self.measured(HIGH_COLLISION)["noise_cohort"], "control")

    def test_the_negative_unique_symbol_is_not_high_noise(self):
        self.assertNotEqual(self.measured(UNIQUE_CONTROL)["noise_cohort"], "high")

    def test_the_two_symbols_are_separated_by_an_order_of_magnitude(self):
        high = self.measured(HIGH_COLLISION)["noise_ratio"]
        control = self.measured(UNIQUE_CONTROL)["noise_ratio"]
        self.assertGreater(high / control, 10.0)


@REQUIRES_CORPUS
class CommittedCohortArtifactTest(unittest.TestCase):
    """The committed cohort set is the contract D3/D4/D5 consume."""

    @classmethod
    def setUpClass(cls):
        path = os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "discovery",
            "cohorts",
            "cohorts.json",
        )
        if not os.path.exists(path):
            raise unittest.SkipTest("cohort artifact not generated yet")
        with open(path) as handle:
            cls.doc = json.load(handle)

    def test_artifact_pins_its_provenance(self):
        self.assertEqual(self.doc["corpus_commit"], PINNED_CORPUS_COMMIT)
        self.assertEqual(self.doc["gopls_version"], PINNED_GOPLS_VERSION)
        self.assertEqual(self.doc["seed"], sc.COHORT_SEED)

    def test_artifact_has_control_cells(self):
        self.assertGreater(len(self.doc["cohorts"]["noise_control"]), 0)

    def test_every_symbol_carries_its_metrics(self):
        for row in self.doc["symbols"]:
            for field in ("noise_ratio", "fan_in", "def_count", "impl_count"):
                self.assertIn(field, row)

    def test_paths_are_corpus_relative(self):
        for row in self.doc["symbols"]:
            self.assertFalse(os.path.isabs(row["path"]), row["path"])


if __name__ == "__main__":
    unittest.main()
