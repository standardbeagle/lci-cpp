"""Tests for the D3 pre-registered prediction registry.

The registry's entire reason to exist is that it is a TIMESTAMPED RECORD written
before D4 runs anything. A prediction written after seeing results is not a
prediction, so these tests police the properties that make the record worth
having:

  * Every family names a falsifiable prediction with a concrete threshold, OR
    is explicitly marked untestable at this sample size. "LCI should do better"
    is not a prediction, and neither is a threshold the sample cannot test.
  * At least two CONTROL families predict parity/grep_wins. A sweep that can
    only report LCI wins is rigged; the controls prove the instrument can
    report a null.
  * Every hypothesis family either names a matched control counterpart or
    records why one is not applicable.

Per .claude/rules/bench-harness-oracle-independence.md, the cell counts are
re-derived HERE from the committed cohort file rather than read out of the
registry's own `n` fields. A registry that asserted its own arithmetic would
share a blind spot with itself: a wrong n would silently justify a wrong power
verdict, which is precisely the failure this slice exists to prevent. The
power-status rules are likewise recomputed from the derived n, not trusted.

Hermetic: reads only two committed JSON files. No network, no corpora, no gopls.

    python3 -m unittest discover -s benchmarks/repo-qa/tests -t .
"""

import json
import os
import unittest

DISCOVERY = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "discovery"
)
REGISTRY_PATH = os.path.join(DISCOVERY, "predictions.json")
COHORTS_PATH = os.path.join(DISCOVERY, "cohorts", "cohorts.json")

VALID_OUTCOMES = {"lci_wins", "parity", "grep_wins"}
VALID_POWER_STATUS = {"powered", "underpowered", "untestable_at_this_n"}
VALID_COMPARISONS = {
    "treatment_minus_baseline",
    "baseline_minus_treatment",
    "abs_treatment_minus_baseline",
}

# Restated here independently of the registry's prose so a silent edit to the
# registry's own derivations cannot make a family self-certify.
#
# There is deliberately NO global "powered" cell-count bar. The n a family needs
# depends on its metric and effect size: the F1-style paired comparisons need
# n≈25, while the rare-event DNF proportion needs n≈900 to detect the prior
# tiers' 4.3pp gap. A single global bar would certify token_budgeted_completion
# (n=48) as powered when it cannot test its own prediction. Power is therefore
# checked as n vs the family's OWN declared min_n_for_80pct_power, with that
# declared value independently sanity-checked per metric below.
UNTESTABLE_MAX_N = 2

# Independent floor on what each metric's min_n_for_80pct_power may claim, so a
# family cannot self-certify as powered by simply declaring a tiny requirement.
MIN_N_FLOOR_BY_METRIC = {
    "mean_f1": 25,
    "mean_precision": 25,
    "mean_recall": 25,
    "dnf_rate_pct": 900,
}


def _load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


# --- Pure rules ------------------------------------------------------------
# The two rules below are the ones that actually caught defects while this
# registry was being written (a lower-is-better metric signed as if it were
# higher-is-better, and a rare-event family self-certifying as powered off a
# global cell-count bar). They are pure functions so DiscriminationTest can
# re-inject those exact known-bad inputs and prove the rules FAIL on them --
# per .claude/rules/bench-harness-oracle-independence.md, a checker with no
# proven failure case is not yet an oracle, however many registries it passes.


def power_status_for(n, min_n_for_80pct_power):
    """Derive power status from a family's own n and its own requirement."""
    if n <= UNTESTABLE_MAX_N:
        return "untestable_at_this_n"
    if n >= min_n_for_80pct_power:
        return "powered"
    return "underpowered"


def expected_comparison(outcome, polarity):
    """Which delta must be positive for `outcome` to hold on this metric."""
    if outcome == "parity":
        return "abs_treatment_minus_baseline"
    winner = {"lci_wins": "treatment", "grep_wins": "baseline"}[outcome]
    loser = "baseline" if winner == "treatment" else "treatment"
    if polarity == "higher_is_better":
        return f"{winner}_minus_{loser}"
    return f"{loser}_minus_{winner}"


class RegistryStructureTest(unittest.TestCase):
    """The registry is machine-readable and D5 can diff outcomes against it."""

    @classmethod
    def setUpClass(cls):
        cls.reg = _load(REGISTRY_PATH)
        cls.families = cls.reg["families"]

    def test_registry_is_machine_readable_with_required_top_level_fields(self):
        for field in (
            "schema_version",
            "registry_id",
            "registered_at",
            "registered_before_any_run",
            "cohort_source",
            "population",
            "arms",
            "primary_metrics",
            "power",
            "families",
        ):
            self.assertIn(field, self.reg, f"registry missing top-level {field!r}")
        self.assertTrue(
            self.reg["registered_before_any_run"],
            "the registry must assert it was registered before any run",
        )

    def test_registry_pins_the_cohort_sample_it_was_written_against(self):
        src = self.reg["cohort_source"]
        cohorts = _load(COHORTS_PATH)
        self.assertEqual(src["seed"], cohorts["seed"])
        self.assertEqual(src["corpus_commit"], cohorts["corpus_commit"])
        self.assertEqual(src["gopls_version"], cohorts["gopls_version"])
        self.assertEqual(src["pool_size"], cohorts["pool_size"])

    def test_population_is_functions_and_methods_only(self):
        # D2's population excludes types and init/main. A prediction that
        # presumed type symbols were in the sample would be untestable by
        # construction, so the registry must pin the population it assumes.
        cohorts = _load(COHORTS_PATH)
        actual_kinds = {s["kind"] for s in cohorts["symbols"]}
        self.assertEqual(
            set(self.reg["population"]["symbol_kinds"]),
            actual_kinds,
            "registry population must match the kinds actually present in the sample",
        )

    def test_family_ids_are_unique_and_non_empty(self):
        ids = [f["id"] for f in self.families]
        self.assertEqual(len(ids), len(set(ids)), "family ids must be unique")
        for fid in ids:
            self.assertTrue(fid.strip(), "family id must be non-empty")

    def test_every_family_carries_the_fields_d5_diffs_against(self):
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                for field in (
                    "id",
                    "title",
                    "role",
                    "task_shape",
                    "cohort",
                    "arm_visible_prompt",
                    "answer_set",
                    "epic_intuition",
                    "prediction",
                    "threshold",
                    "power",
                    "voided",
                ):
                    self.assertIn(field, fam, f"{fam['id']} missing {field!r}")
                self.assertIn(fam["role"], {"hypothesis", "control"})
                self.assertFalse(
                    fam["voided"], f"{fam['id']} is voided; D5 must exclude it"
                )

    def test_every_family_has_a_gradable_answer_set_and_an_arm_visible_prompt(self):
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                self.assertTrue(
                    fam["arm_visible_prompt"].strip(),
                    f"{fam['id']} has no arm-visible prompt",
                )
                ans = fam["answer_set"]
                self.assertTrue(ans["oracle"].strip())
                self.assertTrue(ans["method"].strip())
                self.assertTrue(ans["gradable"], f"{fam['id']} answer set not gradable")
                self.assertTrue(
                    ans["independence_note"].strip(),
                    f"{fam['id']} must record how its oracle stays independent of the arms",
                )


class PredictionFalsifiabilityTest(unittest.TestCase):
    """Every family predicts something that can be WRONG, or says it cannot."""

    @classmethod
    def setUpClass(cls):
        cls.reg = _load(REGISTRY_PATH)
        cls.families = cls.reg["families"]
        cls.metrics = set(cls.reg["primary_metrics"])

    def test_every_prediction_names_an_outcome_and_a_mechanism(self):
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                pred = fam["prediction"]
                self.assertIn(
                    pred["outcome"],
                    VALID_OUTCOMES,
                    f"{fam['id']} outcome must be one of {sorted(VALID_OUTCOMES)}",
                )
                mech = pred["mechanism"]
                self.assertTrue(
                    mech.strip(), f"{fam['id']} must predict WHY, not just what"
                )
                self.assertGreater(
                    len(mech),
                    80,
                    f"{fam['id']} mechanism is too thin to be checkable against an outcome",
                )

    def test_every_family_has_a_threshold_or_an_explicit_untestable_marker(self):
        # This is the criterion the epic's power warning binds hardest: a
        # threshold the sample cannot test is exactly the unfalsifiable hedge
        # the acceptance criteria forbid, so the ONLY way to omit a threshold
        # is to declare the family untestable.
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                status = fam["power"]["status"]
                self.assertIn(status, VALID_POWER_STATUS)
                if fam["threshold"] is None:
                    self.assertEqual(
                        status,
                        "untestable_at_this_n",
                        f"{fam['id']} omits a threshold but is not marked untestable",
                    )
                else:
                    self.assertNotEqual(
                        status,
                        "untestable_at_this_n",
                        f"{fam['id']} is marked untestable but still states a threshold; "
                        "an untestable family must not dress up a threshold its n can never test",
                    )

    def test_stated_thresholds_are_concrete_and_machine_checkable(self):
        for fam in self.families:
            thr = fam["threshold"]
            if thr is None:
                continue
            with self.subTest(family=fam["id"]):
                self.assertIn(thr["metric"], self.metrics, f"{fam['id']} unknown metric")
                self.assertIn(thr["comparison"], VALID_COMPARISONS)
                self.assertIn(thr["operator"], {">=", "<="})
                self.assertIsInstance(thr["value"], (int, float))
                self.assertTrue(
                    thr["falsified_if"].strip(),
                    f"{fam['id']} must state what would falsify it",
                )

    def test_parity_predictions_use_a_two_sided_band(self):
        # A parity prediction is only falsifiable if a win in EITHER direction
        # breaks it. A one-sided parity threshold could not be contradicted by
        # an LCI win, which would make it a hedge.
        for fam in self.families:
            if fam["prediction"]["outcome"] != "parity":
                continue
            with self.subTest(family=fam["id"]):
                thr = fam["threshold"]
                self.assertEqual(thr["comparison"], "abs_treatment_minus_baseline")
                self.assertEqual(thr["operator"], "<=")

    def test_directional_predictions_are_signed_toward_the_predicted_winner(self):
        # Signing depends on METRIC POLARITY, not on the outcome alone. For a
        # lower-is-better metric (dnf_rate_pct) an LCI win means the treatment
        # scores LOWER, so the winning-arm delta flips. Assuming every metric is
        # higher-is-better would demand a threshold predicting the exact
        # OPPOSITE of the family's stated outcome.
        for fam in self.families:
            outcome = fam["prediction"]["outcome"]
            thr = fam["threshold"]
            if thr is None or outcome == "parity":
                continue
            with self.subTest(family=fam["id"]):
                self.assertEqual(thr["operator"], ">=")
                polarity = self.reg["primary_metrics"][thr["metric"]]["polarity"]
                self.assertEqual(
                    thr["comparison"],
                    expected_comparison(outcome, polarity),
                    f"{fam['id']} predicts {outcome} on a {polarity} metric but its "
                    "threshold is not signed toward the predicted winner",
                )
                self.assertGreater(
                    thr["value"], 0, f"{fam['id']} directional threshold must be nonzero"
                )

    def test_every_metric_declares_its_polarity(self):
        for name, spec in self.reg["primary_metrics"].items():
            with self.subTest(metric=name):
                self.assertIn(spec["polarity"], {"higher_is_better", "lower_is_better"})
                self.assertTrue(spec["description"].strip())

    def test_confidence_is_a_probability_or_null_only_when_untestable(self):
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                conf = fam["prediction"]["confidence"]
                if conf is None:
                    self.assertEqual(
                        fam["power"]["status"],
                        "untestable_at_this_n",
                        f"{fam['id']} may only omit confidence if it is untestable",
                    )
                else:
                    self.assertGreater(conf, 0.0)
                    self.assertLess(conf, 1.0)

    def test_epic_intuition_is_recorded_so_d5_can_score_the_intuition_too(self):
        # The user's ask -- "finding where predictions are failing" -- only
        # works if the ORIGINAL intuition is on record next to the calibrated
        # prediction. Where they disagree is the signal.
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                self.assertTrue(fam["epic_intuition"].strip())

    def test_families_contradicting_the_epic_intuition_say_so_explicitly(self):
        # A calibrated prediction that silently overrides the epic's stated
        # intuition would erase the very disagreement D5 is meant to measure.
        for fam in self.families:
            intuition = fam["epic_intuition"]
            outcome = fam["prediction"]["outcome"]
            if intuition in VALID_OUTCOMES and intuition != outcome:
                with self.subTest(family=fam["id"]):
                    note = fam["prediction"].get("note", "")
                    self.assertIn(
                        "CONTRADICT",
                        note.upper(),
                        f"{fam['id']} predicts {outcome} against an intuition of "
                        f"{intuition} without flagging the contradiction",
                    )


class PowerHonestyTest(unittest.TestCase):
    """Power status is re-derived from the committed sample, never trusted."""

    @classmethod
    def setUpClass(cls):
        cls.reg = _load(REGISTRY_PATH)
        cls.families = cls.reg["families"]
        cohorts = _load(COHORTS_PATH)
        cls.cells = cohorts["cohorts"]
        cls.pool_size = cohorts["pool_size"]

    def _derive_n(self, fam):
        """Recompute the cell count from the cohort file itself."""
        cell = fam["cohort"]["cell"]
        if cell == "full_pool":
            return self.pool_size
        parts = [p.strip() for p in cell.split("∩")]
        for part in parts:
            self.assertIn(part, self.cells, f"unknown cohort cell {part!r}")
        derived = set(self.cells[parts[0]])
        for part in parts[1:]:
            derived &= set(self.cells[part])
        return len(derived)

    def test_declared_n_matches_the_committed_cohort_sample(self):
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                self.assertEqual(
                    fam["cohort"]["n"],
                    self._derive_n(fam),
                    f"{fam['id']} declares an n that the committed cohort file "
                    "does not support -- a wrong n would silently justify a wrong "
                    "power verdict",
                )

    def test_power_status_follows_from_the_derived_n(self):
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                n = self._derive_n(fam)
                required = fam["power"]["min_n_for_80pct_power"]
                self.assertEqual(
                    fam["power"]["status"],
                    power_status_for(n, required),
                    f"{fam['id']} n={n}, needs {required}",
                )

    def test_required_n_is_not_understated_for_the_families_metric(self):
        # Guards the rule above: without a floor, a family could self-certify as
        # powered by declaring a min_n it trivially exceeds. The floors are the
        # metric's real requirement, held here independently of the registry.
        for fam in self.families:
            thr = fam["threshold"]
            if thr is None:
                continue
            with self.subTest(family=fam["id"]):
                floor = MIN_N_FLOOR_BY_METRIC[thr["metric"]]
                self.assertGreaterEqual(
                    fam["power"]["min_n_for_80pct_power"],
                    floor,
                    f"{fam['id']} claims to need only "
                    f"{fam['power']['min_n_for_80pct_power']} cells on {thr['metric']}, "
                    f"below that metric's floor of {floor}",
                )

    def test_rare_event_dnf_family_is_not_certified_powered_by_cell_count_alone(self):
        # Discrimination test for the per-metric power rule. token_budgeted_
        # completion has the LARGEST n in the registry (48, the whole pool) and
        # would pass any global n>=25 bar -- yet it cannot detect the 4.3pp gap
        # its own prior predicts. If this ever reports "powered", the power rule
        # has silently reverted to a global cell-count bar.
        dnf = [
            f
            for f in self.families
            if f["threshold"] and f["threshold"]["metric"] == "dnf_rate_pct"
        ]
        self.assertTrue(dnf, "the DNF family must be on the record")
        for fam in dnf:
            with self.subTest(family=fam["id"]):
                self.assertGreater(fam["cohort"]["n"], 25)
                self.assertEqual(fam["power"]["status"], "underpowered")

    def test_every_family_records_n_and_the_power_bars(self):
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                power = fam["power"]
                self.assertEqual(power["n"], fam["cohort"]["n"])
                self.assertIsInstance(power["min_n_for_80pct_power"], int)
                self.assertTrue(
                    power["note"].strip(),
                    f"{fam['id']} must state its power situation in prose D5 can quote",
                )

    def test_underpowered_families_state_the_n_they_would_need(self):
        for fam in self.families:
            if fam["power"]["status"] == "powered":
                continue
            with self.subTest(family=fam["id"]):
                power = fam["power"]
                self.assertGreater(
                    power["min_n_for_80pct_power"],
                    power["n"],
                    f"{fam['id']} is not powered, so the n it needs must exceed the n it has",
                )

    def test_untestable_families_are_retained_not_dropped(self):
        # Silently dropping the underpowered families would hide the gap. The
        # epic's power warning binds this: record them WITH the power problem
        # stated.
        untestable = [
            f for f in self.families if f["power"]["status"] == "untestable_at_this_n"
        ]
        self.assertTrue(
            untestable,
            "impl_many (n=2) is in the sample; a registry that dropped it would "
            "hide the gap the epic's power warning requires be recorded",
        )
        for fam in untestable:
            with self.subTest(family=fam["id"]):
                self.assertIsNone(fam["threshold"])
                self.assertIn("UNTESTABLE", fam["power"]["note"].upper())

    def test_n_equals_two_family_is_marked_untestable(self):
        # Discrimination test for the power rule itself: the known n=2 cell in
        # the committed sample (impl_many) MUST come out untestable. Without
        # this, the power gate could pass while silently classifying nothing.
        self.assertEqual(len(self.cells["impl_many"]), 2, "sample drifted from n=2")
        impl = [f for f in self.families if f["cohort"]["cell"] == "impl_many"]
        self.assertEqual(len(impl), 1, "the impl_many family must be on the record")
        self.assertEqual(impl[0]["power"]["status"], "untestable_at_this_n")
        self.assertIsNone(impl[0]["threshold"])


class ControlCoverageTest(unittest.TestCase):
    """The instrument must be able to report a null, and be shown to."""

    @classmethod
    def setUpClass(cls):
        cls.reg = _load(REGISTRY_PATH)
        cls.families = cls.reg["families"]
        cls.by_id = {f["id"]: f for f in cls.families}

    def test_at_least_two_controls_predict_parity_or_grep_wins(self):
        controls = [
            f
            for f in self.families
            if f["role"] == "control"
            and f["prediction"]["outcome"] in {"parity", "grep_wins"}
        ]
        self.assertGreaterEqual(
            len(controls),
            2,
            "a sweep that can only report LCI wins is rigged; at least two "
            "control families must predict parity/grep_wins",
        )

    def test_at_least_one_control_can_report_a_grep_win(self):
        # Parity controls alone only prove the instrument can report "no
        # difference". Proving it can report an LCI LOSS needs a family whose
        # prediction is directional the other way.
        grep_wins = [
            f for f in self.families if f["prediction"]["outcome"] == "grep_wins"
        ]
        self.assertTrue(
            grep_wins,
            "no family predicts grep_wins; the instrument's ability to report "
            "an LCI loss would be unproven",
        )

    def test_controls_are_powered_enough_to_do_their_job(self):
        # A null check that cannot detect a violation is not a check. At least
        # one control must clear the powered bar.
        powered = [
            f
            for f in self.families
            if f["role"] == "control" and f["power"]["status"] == "powered"
        ]
        self.assertGreaterEqual(
            len(powered),
            2,
            "at least two controls must be powered; an underpowered null check "
            "cannot detect the bias it exists to detect",
        )

    def test_controls_declare_what_they_control_for_and_what_would_break(self):
        for fam in self.families:
            if fam["role"] != "control":
                continue
            with self.subTest(family=fam["id"]):
                self.assertTrue(
                    fam["controls_for"], f"{fam['id']} must name the families it controls"
                )
                for target in fam["controls_for"]:
                    self.assertIn(target, self.by_id, f"{fam['id']} -> unknown {target}")
                self.assertTrue(
                    fam["instrument_check"].strip(),
                    f"{fam['id']} must state what its own violation would mean",
                )

    def test_every_hypothesis_family_names_a_control_or_says_why_not(self):
        for fam in self.families:
            if fam["role"] != "hypothesis":
                continue
            with self.subTest(family=fam["id"]):
                counterpart = fam.get("control_counterpart")
                if counterpart is None:
                    rationale = fam.get("control_counterpart_rationale", "")
                    self.assertGreater(
                        len(rationale),
                        80,
                        f"{fam['id']} has no control counterpart and no substantive "
                        "rationale for its absence",
                    )
                else:
                    self.assertIn(counterpart, self.by_id)
                    self.assertEqual(
                        self.by_id[counterpart]["role"],
                        "control",
                        f"{fam['id']} counterpart must be a control",
                    )

    def test_matched_controls_share_the_task_shape_they_control_for(self):
        # A control only isolates cohort difficulty if the TASK SHAPE is held
        # fixed. A control with a different shape would vary two things at once
        # and could not attribute the difference to the cohort.
        for fam in self.families:
            counterpart = fam.get("control_counterpart")
            if not counterpart:
                continue
            with self.subTest(family=fam["id"]):
                self.assertEqual(
                    fam["task_shape"],
                    self.by_id[counterpart]["task_shape"],
                    f"{fam['id']} and its control {counterpart} must share a task "
                    "shape, or the comparison varies shape and cohort together",
                )

    def test_shared_cell_families_declare_their_non_independence(self):
        # Families drawn from overlapping cells cannot corroborate each other.
        # D5 must not read agreeing outcomes as independent evidence.
        for fam in self.families:
            shared = fam.get("shares_cells_with")
            if not shared:
                continue
            with self.subTest(family=fam["id"]):
                for other in shared:
                    self.assertIn(other, self.by_id)
                    self.assertIn(
                        fam["id"],
                        self.by_id[other].get("shares_cells_with", []),
                        f"{fam['id']} claims to share cells with {other} but the "
                        "relation is not declared on both sides",
                    )


class AmendmentPolicyTest(unittest.TestCase):
    """The record's value is that it cannot be quietly retuned afterwards."""

    def test_registry_states_the_no_edit_after_run_policy(self):
        reg = _load(REGISTRY_PATH)
        policy = reg["amendment_policy"]
        self.assertIn("MUST NOT be edited", policy)
        self.assertIn("void", policy.lower())

    def test_every_family_carries_a_void_flag_for_the_amendment_path(self):
        # Voiding is the ONLY sanctioned post-hoc edit, so the field must exist
        # on every family up front -- adding it later would itself be a retune.
        for fam in _load(REGISTRY_PATH)["families"]:
            with self.subTest(family=fam["id"]):
                self.assertIn("voided", fam)
                self.assertIsInstance(fam["voided"], bool)


class DiscriminationTest(unittest.TestCase):
    """Prove the gates FAIL on the bad registries they exist to catch.

    Every case here is a defect that either actually occurred while writing this
    registry, or is the specific hedge the acceptance criteria forbid. A gate
    that has only ever been observed passing is unverified.
    """

    # --- metric polarity ---------------------------------------------------

    def test_polarity_rule_rejects_dnf_signed_as_if_higher_were_better(self):
        # THE ACTUAL BUG: signing an lci_wins DNF prediction as
        # treatment_minus_baseline predicts LCI does MORE not-finishing --
        # the exact opposite of the stated outcome.
        wrong = "treatment_minus_baseline"
        self.assertEqual(
            expected_comparison("lci_wins", "lower_is_better"),
            "baseline_minus_treatment",
        )
        self.assertNotEqual(expected_comparison("lci_wins", "lower_is_better"), wrong)

    def test_polarity_rule_separates_the_two_polarities_in_both_directions(self):
        # Both directions, or the rule could be a constant that happens to fit.
        self.assertEqual(
            expected_comparison("lci_wins", "higher_is_better"),
            "treatment_minus_baseline",
        )
        self.assertEqual(
            expected_comparison("grep_wins", "higher_is_better"),
            "baseline_minus_treatment",
        )
        self.assertEqual(
            expected_comparison("grep_wins", "lower_is_better"),
            "treatment_minus_baseline",
        )

    # --- power -------------------------------------------------------------

    def test_power_rule_refuses_to_certify_the_rare_event_family_at_n48(self):
        # THE ACTUAL BUG: a global n>=25 bar certified n=48 as powered when the
        # DNF metric needs ~900.
        self.assertEqual(power_status_for(48, 900), "underpowered")

    def test_power_rule_certifies_only_when_n_meets_the_metrics_own_bar(self):
        self.assertEqual(power_status_for(26, 25), "powered")
        self.assertEqual(power_status_for(24, 25), "underpowered")
        self.assertEqual(power_status_for(16, 25), "underpowered")

    def test_power_rule_calls_n2_untestable_however_low_the_bar_is_set(self):
        # n=2 must be untestable even if a family declared a bar it exceeds:
        # at n=2 the smallest attainable two-sided sign-test p is 0.50, so no
        # requirement can rescue it.
        self.assertEqual(power_status_for(2, 1), "untestable_at_this_n")
        self.assertEqual(power_status_for(2, 25), "untestable_at_this_n")

    def test_min_n_floor_rejects_a_family_understating_what_its_metric_needs(self):
        # Self-certification path: declare min_n=6 on an F1 family with n=6 and
        # the status rule alone would happily call it powered.
        self.assertEqual(power_status_for(6, 6), "powered")
        self.assertLess(6, MIN_N_FLOOR_BY_METRIC["mean_f1"])  # the floor catches it

    # --- registry-level gates ----------------------------------------------

    def _real(self):
        return _load(REGISTRY_PATH)

    def test_threshold_gate_rejects_an_untestable_family_wearing_a_threshold(self):
        # The forbidden hedge: dressing an n=2 family up with a numeric
        # threshold its sample can never test.
        reg = self._real()
        fam = next(
            f for f in reg["families"] if f["power"]["status"] == "untestable_at_this_n"
        )
        fam["threshold"] = {
            "metric": "mean_f1",
            "comparison": "treatment_minus_baseline",
            "operator": ">=",
            "value": 0.20,
            "falsified_if": "looks rigorous, tests nothing",
        }
        offenders = [
            f["id"]
            for f in reg["families"]
            if f["power"]["status"] == "untestable_at_this_n"
            and f["threshold"] is not None
        ]
        self.assertIn(fam["id"], offenders)

    def test_threshold_gate_rejects_a_missing_threshold_without_the_marker(self):
        reg = self._real()
        fam = next(f for f in reg["families"] if f["threshold"] is not None)
        fam["threshold"] = None
        offenders = [
            f["id"]
            for f in reg["families"]
            if f["threshold"] is None
            and f["power"]["status"] != "untestable_at_this_n"
        ]
        self.assertIn(fam["id"], offenders)

    def test_control_gate_rejects_a_registry_with_only_lci_win_predictions(self):
        # The rigged sweep this criterion exists to prevent.
        reg = self._real()
        for fam in reg["families"]:
            fam["role"] = "hypothesis"
            fam["prediction"]["outcome"] = "lci_wins"
        controls = [
            f
            for f in reg["families"]
            if f["role"] == "control"
            and f["prediction"]["outcome"] in {"parity", "grep_wins"}
        ]
        self.assertEqual(len(controls), 0)
        self.assertLess(len(controls), 2, "a rigged registry must fail the control gate")

    def test_declared_n_gate_rejects_an_n_the_cohort_file_does_not_support(self):
        # Inflating n is how an underpowered family would launder itself into a
        # powered verdict, so the derived-n check must catch a wrong n.
        cohorts = _load(COHORTS_PATH)
        reg = self._real()
        fam = next(f for f in reg["families"] if f["cohort"]["cell"] == "impl_many")
        fam["cohort"]["n"] = 30
        self.assertNotEqual(fam["cohort"]["n"], len(cohorts["cohorts"]["impl_many"]))
        self.assertEqual(power_status_for(30, 25), "powered")  # the lie would work
        self.assertEqual(  # the truth does not
            power_status_for(len(cohorts["cohorts"]["impl_many"]), 25),
            "untestable_at_this_n",
        )


if __name__ == "__main__":
    unittest.main()
