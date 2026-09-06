"""Discrimination tests for the D4 two-arm discovery sweep.

Every test here is hermetic: no repo is cloned, no corpus content is committed,
no agent or MCP server is started. Arm execution is injected as a callable so
the runner's grading, resume-safety, and failure handling are provable without
a model in the loop.

The three cases the task binds explicitly are:
  * a grader handed a PARTIAL caller set must not score as correct,
  * a DNF must survive into the output rather than being dropped,
  * a broken cell must raise instead of emitting a zeroed row.
"""

import json
import os
import sys
import tempfile
import unittest

BENCH_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

import importlib
import importlib.util

# `discovery/runner` and `exploration/runner` are both named `runner`. Putting
# either directory on sys.path shadows the other for the whole process, so this
# package is loaded from its path under a unique module name instead.
_RUNNER_DIR = os.path.join(BENCH_ROOT, "discovery", "runner")


def _load_discovery_runner():
    if "discovery_runner" not in sys.modules:
        spec = importlib.util.spec_from_file_location(
            "discovery_runner",
            os.path.join(_RUNNER_DIR, "__init__.py"),
            submodule_search_locations=[_RUNNER_DIR],
        )
        module = importlib.util.module_from_spec(spec)
        sys.modules["discovery_runner"] = module
        spec.loader.exec_module(module)
    return [importlib.import_module("discovery_runner." + name)
            for name in ("answer_sets", "cells", "grading", "sweep")]


answer_sets, cells, grading, sweep = _load_discovery_runner()


# --- fixtures ---------------------------------------------------------------


def _symbol(slug, name, path, line, column, **over):
    rec = {
        "slug": slug,
        "name": name,
        "path": path,
        "line": line,
        "column": column,
        "kind": "func",
        "def_count": 1,
        "fan_in": 2,
        "grep_hit_count": 4,
        "impl_count": 0,
        "noise_ratio": 1.0,
        "noise_cohort": "control",
        "production_reference_count": 2,
        "test_reference_count": 0,
        "test_reference_share": 0.0,
        "true_reference_count": 2,
    }
    rec.update(over)
    return rec


def _cohorts(symbols, cohorts=None):
    return {
        "schema_version": 1,
        "corpus_commit": "c0ffee",
        "gopls_version": "gopls v0.23.0",
        "seed": "test-seed",
        "pool_size": len(symbols),
        "candidate_count": len(symbols),
        "skipped": [],
        "thresholds": {"control_max_noise": 2.0, "high_min_noise": 8.0},
        "symbols": symbols,
        "cohorts": cohorts or {},
    }


def _family(fid, shape, selector, n, **over):
    fam = {
        "id": fid,
        "title": fid,
        "role": "hypothesis",
        "task_shape": shape,
        "cohort": {"axis": "test", "cell": "test", "n": n, "selector": selector},
        "arm_visible_prompt": "find {{symbol_name}} at {{file_path}}:{{line}}",
        "answer_set": {"oracle": "gopls", "gradable": True},
        "prediction": {"outcome": "lci_wins"},
        "threshold": {"metric": "mean_f1", "comparison": "treatment_minus_baseline",
                      "operator": ">=", "value": 0.2},
        "voided": False,
    }
    fam.update(over)
    return fam


def _registry(families):
    return {
        "schema_version": 2,
        "registry_id": "test-registry",
        "arms": {"treatment": {"tools": ["mcp__lci__callers"]},
                 "baseline": {"tools": ["Grep"]}},
        "families": families,
    }


def _key(callers=(), definition=None, references=(), implementations=()):
    return {
        "schema_version": 1,
        "corpus_commit": "c0ffee",
        "gopls_version": "gopls v0.23.0",
        "symbol": {"name": "Target", "path": "a.go", "line": 10, "column": 6},
        "max_depth": 2,
        "definition": definition or {"path": "a.go", "line": 10, "column": 6,
                                     "kind": "production", "external": False},
        "references": list(references),
        "callers": list(callers),
        "implementations": list(implementations),
        "reference_counts": {"total": len(references), "test": 0,
                             "production": len(references)},
    }


def _caller(path, line, site_path, site_line, depth=1):
    return {
        "name": "Caller",
        "path": path,
        "line": line,
        "column": 6,
        "external": False,
        "kind": "production",
        "depth": depth,
        "call_site": {"path": site_path, "line": site_line, "column": 3,
                      "kind": "production"},
    }


TWELVE_CALLERS = [
    _caller("c%d.go" % i, 100 + i, "c%d.go" % i, 200 + i) for i in range(12)
]


# --- grading ---------------------------------------------------------------


class GradingDiscriminationTest(unittest.TestCase):
    def setUp(self):
        self.truth = answer_sets.answer_set_for("exhaustive_callers", _key(TWELVE_CALLERS))

    def test_partial_caller_set_is_not_correct(self):
        """3 of 12 real callers: perfect precision, but NOT a correct answer."""
        answer = "\n".join("c%d.go:%d" % (i, 200 + i) for i in range(3))
        score = grading.score_answer(answer, self.truth)
        self.assertEqual(score["precision"], 1.0)
        self.assertAlmostEqual(score["recall"], 3 / 12)
        self.assertLess(score["f1"], 1.0)
        self.assertFalse(score["correct"])

    def test_complete_set_is_correct(self):
        answer = "\n".join("c%d.go:%d" % (i, 200 + i) for i in range(12))
        score = grading.score_answer(answer, self.truth)
        self.assertEqual(score["recall"], 1.0)
        self.assertTrue(score["correct"])

    def test_uncited_prose_earns_no_credit(self):
        score = grading.score_answer(
            "It is called from twelve places throughout the codebase.", self.truth
        )
        self.assertEqual(score["cited_total"], 0)
        self.assertEqual(score["precision"], 0.0)
        self.assertEqual(score["recall"], 0.0)
        self.assertFalse(score["correct"])

    def test_noise_costs_precision(self):
        answer = "\n".join("c%d.go:%d" % (i, 200 + i) for i in range(12))
        answer += "\nunrelated.go:9\nother.go:4"
        score = grading.score_answer(answer, self.truth)
        self.assertEqual(score["recall"], 1.0)
        self.assertAlmostEqual(score["precision"], 12 / 14)
        self.assertFalse(score["correct"])

    def test_empty_answer_set_is_ungradable_not_zero(self):
        """fan_in=0 cells: F1 is undefined on an empty oracle set."""
        truth = answer_sets.answer_set_for("exhaustive_callers", _key([]))
        self.assertFalse(truth["gradable"])
        score = grading.score_answer("There are no call sites.", truth)
        self.assertIsNone(score["f1"])
        self.assertEqual(score["ungradable_reason"], grading.REASON_EMPTY_ANSWER_SET)
        self.assertTrue(score["reported_empty"])


class AnswerSetTest(unittest.TestCase):
    def test_call_sites_not_caller_definitions(self):
        key = _key([_caller("def.go", 11, "site.go", 42)])
        truth = answer_sets.answer_set_for("exhaustive_callers", key)
        self.assertEqual(truth["locations"], [("site.go", 42)])

    def test_depth2_grades_caller_definitions(self):
        key = _key([_caller("d1.go", 11, "s1.go", 42, depth=1),
                    _caller("d2.go", 21, "s2.go", 52, depth=2)])
        truth = answer_sets.answer_set_for("transitive_callers_depth2", key)
        self.assertEqual(truth["locations"], [("d1.go", 11), ("d2.go", 21)])

    def test_depth1_family_excludes_depth2_callers(self):
        key = _key([_caller("d1.go", 11, "s1.go", 42, depth=1),
                    _caller("d2.go", 21, "s2.go", 52, depth=2)])
        truth = answer_sets.answer_set_for("exhaustive_callers", key)
        self.assertEqual(truth["locations"], [("s1.go", 42)])

    def test_production_partition_drops_test_references(self):
        key = _key(references=[
            {"path": "a.go", "line": 3, "kind": "production"},
            {"path": "a_test.go", "line": 7, "kind": "test"},
        ])
        truth = answer_sets.answer_set_for("production_reference_partition", key)
        self.assertEqual(truth["locations"], [("a.go", 3)])

    def test_budget_variant_has_no_answer_set(self):
        truth = answer_sets.answer_set_for("budget_constrained_variant", _key())
        self.assertFalse(truth["gradable"])
        self.assertEqual(truth["ungradable_reason"], grading.REASON_NOT_ANSWER_SET_GRADED)

    def test_unknown_task_shape_fails_loud(self):
        with self.assertRaises(answer_sets.UnknownTaskShape):
            answer_sets.answer_set_for("telepathy", _key())

    def test_literal_scan_is_independent_of_gopls(self):
        with tempfile.TemporaryDirectory() as root:
            with open(os.path.join(root, "a.go"), "w") as fh:
                fh.write("package a\n// mentions Target\nvar Target = 1\n")
            truth = answer_sets.literal_answer_set(root, "Target")
        self.assertEqual(truth["locations"], [("a.go", 2), ("a.go", 3)])


# --- cell planning ----------------------------------------------------------


class PlanTest(unittest.TestCase):
    def setUp(self):
        self.symbols = [
            _symbol("s0", "A", "a.go", 1, 6, fan_in=1, grep_hit_count=45),
            _symbol("s1", "B", "b.go", 2, 6, fan_in=4, grep_hit_count=40),
            _symbol("s2", "C", "c.go", 3, 6, fan_in=8, grep_hit_count=8),
            _symbol("s3", "D", "d.go", 4, 6, fan_in=0, grep_hit_count=14),
        ]
        self.cohorts = _cohorts(self.symbols, {"cell_x": ["s1", "s2"]})

    def test_derived_call_site_noise_selector(self):
        fam = _family("f", "exhaustive_callers",
                      {"kind": "derived_call_site_noise", "operator": ">=",
                       "value": 8.0, "requires_fan_in_at_least": 1}, 2)
        plan = cells.build_plan(_registry([fam]), self.cohorts)
        self.assertEqual([c["slug"] for c in plan["families"][0]["cells"]], ["s0", "s1"])

    def test_cohort_cell_selector(self):
        fam = _family("f", "definition_lookup",
                      {"kind": "cohort_cell", "cell": "cell_x"}, 2)
        plan = cells.build_plan(_registry([fam]), self.cohorts)
        self.assertEqual([c["slug"] for c in plan["families"][0]["cells"]], ["s1", "s2"])

    def test_declared_n_mismatch_fails_loud(self):
        fam = _family("f", "definition_lookup",
                      {"kind": "cohort_cell", "cell": "cell_x"}, 5)
        with self.assertRaises(cells.PlanError):
            cells.build_plan(_registry([fam]), self.cohorts)

    def test_bigger_pool_needs_no_code_change(self):
        """D2b re-registers a larger pool: same code, more cells."""
        bigger = self.symbols + [
            _symbol("s%d" % i, "N%d" % i, "n%d.go" % i, i, 6) for i in range(4, 20)
        ]
        cohorts = _cohorts(bigger, {"cell_x": ["s%d" % i for i in range(4, 20)]})
        fam = _family("f", "definition_lookup",
                      {"kind": "cohort_cell", "cell": "cell_x"}, 16)
        plan = cells.build_plan(_registry([fam]), cohorts)
        self.assertEqual(len(plan["families"][0]["cells"]), 16)

    def test_reduction_knob_is_reported_never_silent(self):
        fam = _family("f", "definition_lookup",
                      {"kind": "cohort_cell", "cell": "cell_x"}, 2)
        plan = cells.build_plan(_registry([fam]), self.cohorts, max_cells_per_family=1)
        reduction = plan["cohort_reduction"]
        self.assertEqual(reduction["max_cells_per_family"], 1)
        self.assertEqual(reduction["families"]["f"]["declared_n"], 2)
        self.assertEqual(reduction["families"]["f"]["ran_n"], 1)
        self.assertEqual(reduction["families"]["f"]["dropped"], ["s2"])

    def test_no_reduction_records_none(self):
        fam = _family("f", "definition_lookup",
                      {"kind": "cohort_cell", "cell": "cell_x"}, 2)
        plan = cells.build_plan(_registry([fam]), self.cohorts)
        self.assertIsNone(plan["cohort_reduction"]["max_cells_per_family"])
        self.assertEqual(plan["cohort_reduction"]["families"], {})

    def test_unfilled_prompt_token_fails_loud(self):
        """A prompt field the cohort file does not publish is a finding."""
        fam = _family("f", "definition_lookup",
                      {"kind": "cohort_cell", "cell": "cell_x"}, 2,
                      arm_visible_prompt="find {{symbol_name}} on {{receiver_type}}")
        with self.assertRaises(cells.PlanError) as ctx:
            cells.build_plan(_registry([fam]), self.cohorts)
        self.assertIn("receiver_type", str(ctx.exception))

    def test_voided_family_is_skipped_and_named(self):
        fam = _family("f", "definition_lookup",
                      {"kind": "cohort_cell", "cell": "cell_x"}, 2,
                      voided=True, void_reason="prompt broken")
        plan = cells.build_plan(_registry([fam]), self.cohorts)
        self.assertEqual(plan["families"], [])
        self.assertEqual(plan["voided"], [{"family": "f", "reason": "prompt broken"}])


# --- the sweep --------------------------------------------------------------


class _Executor:
    """Scripted arm executor: maps (level, arm) -> canned result."""

    def __init__(self, script):
        self.script = script
        self.calls = []

    def __call__(self, job):
        self.calls.append((job["level"], job["arm"], job["slug"]))
        return dict(self.script[(job["level"], job["arm"])])


def _sweep_fixture():
    symbols = [_symbol("s1", "B", "b.go", 2, 6), _symbol("s2", "C", "c.go", 3, 6)]
    cohorts = _cohorts(symbols, {"cell_x": ["s1", "s2"]})
    fam = _family("f", "exhaustive_callers", {"kind": "cohort_cell", "cell": "cell_x"}, 2)
    registry = _registry([fam])
    keys = {"s1": _key(TWELVE_CALLERS), "s2": _key(TWELVE_CALLERS)}
    return registry, cohorts, keys


def _full_answer():
    return "\n".join("c%d.go:%d" % (i, 200 + i) for i in range(12))


class SweepTest(unittest.TestCase):
    def setUp(self):
        self.registry, self.cohorts, self.keys = _sweep_fixture()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.run_dir = os.path.join(self.tmp.name, "run")

    def _run(self, executor, run_dir=None):
        plan = cells.build_plan(self.registry, self.cohorts)
        return sweep.run_sweep(
            plan,
            answer_key_for=lambda cell: self.keys[cell["slug"]],
            executor=executor,
            run_dir=run_dir or self.run_dir,
        )

    def test_broken_cell_raises_and_writes_no_row(self):
        ex = _Executor({
            ("tool", "treatment"): {"status": "error", "detail": "mcp server died"},
            ("tool", "baseline"): {"status": "ok", "answer": ""},
            ("agent", "treatment"): {"status": "ok", "answer": ""},
            ("agent", "baseline"): {"status": "ok", "answer": ""},
        })
        with self.assertRaises(sweep.BrokenCellError) as ctx:
            self._run(ex)
        self.assertIn("mcp server died", str(ctx.exception))
        written = [n for n in os.listdir(self.run_dir)] if os.path.isdir(self.run_dir) else []
        self.assertNotIn("f__treatment__tool__s1.json", written)

    def test_dnf_is_a_first_class_outcome(self):
        ex = _Executor({
            ("tool", "treatment"): {"status": "ok", "answer": _full_answer()},
            ("tool", "baseline"): {"status": "ok", "answer": _full_answer()},
            ("agent", "treatment"): {"status": "dnf", "reason": "timeout"},
            ("agent", "baseline"): {"status": "ok", "answer": _full_answer()},
        })
        self._run(ex)
        report = sweep.build_report(self.run_dir)
        agent = report["levels"]["agent"]["f"]
        self.assertEqual(agent["treatment"]["dnf_count"], 2)
        self.assertEqual(agent["treatment"]["dnf_rate_pct"], 100.0)
        self.assertEqual(agent["baseline"]["dnf_rate_pct"], 0.0)
        with open(os.path.join(self.run_dir, "f__treatment__agent__s1.json")) as fh:
            row = json.load(fh)
        self.assertEqual(row["status"], "dnf")
        self.assertIsNone(row["score"]["f1"])

    def test_dnf_is_excluded_from_mean_not_scored_as_zero(self):
        ex = _Executor({
            ("tool", "treatment"): {"status": "ok", "answer": _full_answer()},
            ("tool", "baseline"): {"status": "ok", "answer": _full_answer()},
            ("agent", "treatment"): {"status": "dnf", "reason": "timeout"},
            ("agent", "baseline"): {"status": "ok", "answer": _full_answer()},
        })
        self._run(ex)
        report = sweep.build_report(self.run_dir)
        treatment = report["levels"]["agent"]["f"]["treatment"]
        self.assertIsNone(treatment["mean_f1"])
        self.assertEqual(treatment["graded_count"], 0)

    def test_resume_is_idempotent(self):
        ex = _Executor({
            ("tool", "treatment"): {"status": "ok", "answer": _full_answer()},
            ("tool", "baseline"): {"status": "ok", "answer": _full_answer()},
            ("agent", "treatment"): {"status": "ok", "answer": _full_answer()},
            ("agent", "baseline"): {"status": "ok", "answer": _full_answer()},
        })
        first = self._run(ex)
        first_calls = len(ex.calls)
        self.assertEqual(first["executed"], first_calls)
        before = sorted(os.listdir(self.run_dir))
        second = self._run(ex)
        self.assertEqual(len(ex.calls), first_calls, "resume re-executed completed cells")
        self.assertEqual(second["executed"], 0)
        self.assertEqual(second["resumed"], first_calls)
        self.assertEqual(sorted(os.listdir(self.run_dir)), before)

    def test_both_levels_reported_separately(self):
        ex = _Executor({
            ("tool", "treatment"): {"status": "ok", "answer": _full_answer()},
            ("tool", "baseline"): {"status": "ok", "answer": "c0.go:200"},
            ("agent", "treatment"): {"status": "ok", "answer": "c0.go:200"},
            ("agent", "baseline"): {"status": "ok", "answer": "c0.go:200"},
        })
        self._run(ex)
        report = sweep.build_report(self.run_dir)
        self.assertEqual(report["levels"]["tool"]["f"]["treatment"]["mean_f1"], 1.0)
        self.assertLess(report["levels"]["agent"]["f"]["treatment"]["mean_f1"], 1.0)

    def test_tool_correct_but_no_agent_lift_is_named(self):
        ex = _Executor({
            ("tool", "treatment"): {"status": "ok", "answer": _full_answer()},
            ("tool", "baseline"): {"status": "ok", "answer": "c0.go:200"},
            ("agent", "treatment"): {"status": "ok", "answer": "c0.go:200"},
            ("agent", "baseline"): {"status": "ok", "answer": "c0.go:200"},
        })
        self._run(ex)
        report = sweep.build_report(self.run_dir)
        self.assertIn("f", report["tool_correct_no_agent_lift"])

    def test_agent_lift_suppresses_the_adoption_flag(self):
        ex = _Executor({
            ("tool", "treatment"): {"status": "ok", "answer": _full_answer()},
            ("tool", "baseline"): {"status": "ok", "answer": "c0.go:200"},
            ("agent", "treatment"): {"status": "ok", "answer": _full_answer()},
            ("agent", "baseline"): {"status": "ok", "answer": "c0.go:200"},
        })
        self._run(ex)
        report = sweep.build_report(self.run_dir)
        self.assertEqual(report["tool_correct_no_agent_lift"], [])

    def test_both_arms_run_every_cell_at_both_levels(self):
        ex = _Executor({
            ("tool", "treatment"): {"status": "ok", "answer": _full_answer()},
            ("tool", "baseline"): {"status": "ok", "answer": _full_answer()},
            ("agent", "treatment"): {"status": "ok", "answer": _full_answer()},
            ("agent", "baseline"): {"status": "ok", "answer": _full_answer()},
        })
        self._run(ex)
        self.assertEqual(len(ex.calls), 2 * 2 * 2)
        rows = [n for n in os.listdir(self.run_dir) if n != sweep.PLAN_FILE]
        self.assertEqual(len(rows), 8)

    def test_report_carries_the_reduction_record(self):
        plan = cells.build_plan(self.registry, self.cohorts, max_cells_per_family=1)
        ex = _Executor({
            ("tool", "treatment"): {"status": "ok", "answer": _full_answer()},
            ("tool", "baseline"): {"status": "ok", "answer": _full_answer()},
            ("agent", "treatment"): {"status": "ok", "answer": _full_answer()},
            ("agent", "baseline"): {"status": "ok", "answer": _full_answer()},
        })
        sweep.run_sweep(plan, answer_key_for=lambda c: self.keys[c["slug"]],
                        executor=ex, run_dir=self.run_dir)
        report = sweep.build_report(self.run_dir)
        self.assertEqual(report["cohort_reduction"]["max_cells_per_family"], 1)
        self.assertEqual(report["cohort_reduction"]["families"]["f"]["ran_n"], 1)

    def test_oracle_failure_is_a_broken_cell(self):
        def boom(cell):
            raise RuntimeError("gopls-oracle FAILED: [GOPLS_MISSING]")

        plan = cells.build_plan(self.registry, self.cohorts)
        ex = _Executor({(lvl, arm): {"status": "ok", "answer": ""}
                        for lvl in ("tool", "agent") for arm in ("treatment", "baseline")})
        with self.assertRaises(sweep.BrokenCellError):
            sweep.run_sweep(plan, answer_key_for=boom, executor=ex, run_dir=self.run_dir)


if __name__ == "__main__":
    unittest.main()
