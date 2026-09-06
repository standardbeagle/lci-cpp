"""Tests for the D3 pre-registered prediction registry.

The registry's entire reason to exist is that it is a TIMESTAMPED RECORD written
before D4 runs anything. A prediction written after seeing results is not a
prediction, so these tests police the properties that make the record worth
having:

  * Every family names a falsifiable prediction with a concrete threshold, OR
    is explicitly marked untestable at this sample size. "LCI should do better"
    is not a prediction, and neither is a threshold the sample cannot test.
  * Every family's difficulty axis divides by the same thing its answer set
    counts. This is the gate that would have caught the schema_version 1 defect
    (see AxisGroundingTest).
  * At least two CONTROL families predict parity/grep_wins. A sweep that can
    only report LCI wins is rigged; the controls prove the instrument can
    report a null.
  * Every hypothesis family either names a matched control counterpart or
    records why one is not applicable.

Per .claude/rules/bench-harness-oracle-independence.md, cell membership is
re-derived HERE from the committed cohort file rather than read out of the
registry's own `n` fields, and the rules that do the gating are pure functions
that DiscriminationTest re-injects known-bad inputs into. A registry that
asserted its own arithmetic would share a blind spot with itself; a gate only
ever observed passing is unverified.

Hermetic: reads only two committed JSON files. No network, no corpora, no gopls.

    python3 -m unittest discover -s benchmarks/repo-qa/tests -t .
"""

import copy
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

# Below n=6 NO split can reach two-sided p<0.05 under a sign test: the best
# attainable at n=5 is 2*(0.5^5)=0.063. Untestability at n<6 is therefore
# arithmetic, not judgement, and no declared requirement can rescue such a
# family. Restated here independently of the registry's own prose.
MIN_N_THAT_CAN_REACH_SIGNIFICANCE = 6

# There is deliberately NO global "powered" cell-count bar: the n a family needs
# depends on its metric. The F1-style paired comparisons need n≈25, while the
# rare-event DNF proportion needs n≈900 to detect the prior tiers' 4.3pp gap. A
# single global bar would certify token_budgeted_completion (n=48) as powered
# when it cannot test its own prediction. Power is checked as n vs the family's
# OWN declared min_n_for_80pct_power, with that declaration floored per metric
# so a family cannot self-certify by declaring a bar it trivially exceeds.
MIN_N_FLOOR_BY_METRIC = {
    "mean_f1": 25,
    "mean_precision": 25,
    "mean_recall": 25,
    "dnf_rate_pct": 900,
}

# A family's difficulty axis MUST divide by the same thing its answer set
# counts. Mapping task shape -> required axis is what makes that checkable.
REQUIRED_AXIS_BY_TASK_SHAPE = {
    "exhaustive_callers": "call_site_noise",
    "definition_lookup": "def_count",
    "production_reference_partition": "refs",
    "implementations_lookup": "impl",
}
# Shapes whose difficulty is not a per-symbol ratio at all.
AXIS_EXEMPT_TASK_SHAPES = {"literal_string_search", "budget_constrained_variant"}
# Shapes graded on call sites but cohorted on another axis: allowed only with a
# declared confound, since the axis and the answer set disagree by construction.
CONFOUND_ALLOWED_TASK_SHAPES = {"transitive_callers_depth2"}


def _load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


# --- Pure rules ------------------------------------------------------------
# Each rule below is a gate DiscriminationTest re-injects known-bad input into.
# Three of them exist because the defect they catch actually shipped to review.


def call_site_noise(sym):
    """Grep hits the baseline must sift per TRUE CALL SITE.

    Undefined at fan_in=0: a symbol with no callers has no per-call-site ratio,
    which is why the zero-caller cells are excluded from call-site families by
    rule rather than by preference.
    """
    if sym["fan_in"] < 1:
        raise ZeroDivisionError(f"call_site_noise undefined at fan_in=0: {sym['slug']}")
    return sym["grep_hit_count"] / sym["fan_in"]


def reference_noise(sym):
    """D2's noise_ratio: grep hits per TRUE REFERENCE. Correct only where the
    answer set is references -- NOT for a callers task."""
    return sym["noise_ratio"]


def derive_members(selector, cohorts):
    """Re-derive a family's cell membership from the committed cohort file."""
    kind = selector["kind"]
    cells = cohorts["cohorts"]
    symbols = cohorts["symbols"]
    if kind == "full_pool":
        return {s["slug"] for s in symbols}
    if kind == "cohort_cell":
        return set(cells[selector["cell"]])
    if kind == "intersect":
        out = set(cells[selector["cells"][0]])
        for cell in selector["cells"][1:]:
            out &= set(cells[cell])
        return out
    if kind == "derived_call_site_noise":
        floor = selector["requires_fan_in_at_least"]
        op = selector["operator"]
        val = selector["value"]
        out = set()
        for sym in symbols:
            if sym["fan_in"] < floor:
                continue
            ratio = call_site_noise(sym)
            if (op == ">=" and ratio >= val) or (op == "<=" and ratio <= val):
                out.add(sym["slug"])
        return out
    raise AssertionError(f"unknown selector kind {kind!r}")


def power_status_for(n, min_n_for_80pct_power):
    """Derive power status from a family's own n and its own requirement."""
    if n < MIN_N_THAT_CAN_REACH_SIGNIFICANCE:
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


def threshold_marker_violation(fam):
    """A family may omit a threshold ONLY by declaring itself untestable, and an
    untestable family may not carry one."""
    untestable = fam["power"]["status"] == "untestable_at_this_n"
    if fam["threshold"] is None and not untestable:
        return f"{fam['id']}: no threshold and no untestable marker"
    if fam["threshold"] is not None and untestable:
        return f"{fam['id']}: untestable but states a threshold its n can never test"
    return None


def axis_violation(fam):
    """The family's difficulty axis must match what its answer set counts."""
    shape = fam["task_shape"]
    if shape in AXIS_EXEMPT_TASK_SHAPES:
        return None
    axis = fam["cohort"]["axis"]
    if shape in CONFOUND_ALLOWED_TASK_SHAPES:
        note = fam["prediction"].get("note", "")
        if "CONFOUND" not in note.upper():
            return f"{fam['id']}: cohorted on {axis!r} but graded on call sites, with no declared confound"
        return None
    required = REQUIRED_AXIS_BY_TASK_SHAPE.get(shape)
    if required is None:
        return f"{fam['id']}: task_shape {shape!r} has no required axis"
    if axis != required:
        return (
            f"{fam['id']}: task_shape {shape!r} grades against a denominator that "
            f"axis {axis!r} does not divide by; requires {required!r}"
        )
    return None


def control_gate_violation(families):
    """>=2 controls predicting parity/grep_wins, and >=1 able to report a grep win."""
    controls = [
        f
        for f in families
        if f["role"] == "control" and f["prediction"]["outcome"] in {"parity", "grep_wins"}
    ]
    if len(controls) < 2:
        return f"only {len(controls)} control(s) predict parity/grep_wins; a sweep that can only report LCI wins is rigged"
    if not [f for f in families if f["prediction"]["outcome"] == "grep_wins"]:
        return "no family predicts grep_wins; the instrument's ability to report an LCI loss is unproven"
    return None


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
            "difficulty_axes",
            "primary_metrics",
            "power",
            "registry_findings",
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
        cohorts = _load(COHORTS_PATH)
        self.assertEqual(
            set(self.reg["population"]["symbol_kinds"]),
            {s["kind"] for s in cohorts["symbols"]},
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

    def test_the_grep_arm_is_never_graded_by_a_grep_oracle(self):
        # The baseline's whole mechanism is grep(1). Grading it with a grep
        # oracle would mean the arm is graded by its own matching mechanism --
        # the shared-blind-spot failure the oracle-independence rule forbids --
        # and would bias the one control that proves LCI can lose.
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                self.assertNotIn(
                    fam["answer_set"]["oracle"],
                    {"grep", "grep(1)", "ripgrep"},
                    f"{fam['id']} grades a grep arm with a grep oracle",
                )


class AxisGroundingTest(unittest.TestCase):
    """A family's difficulty statistic must divide by what its answer set counts.

    This is the gate that schema_version 1 lacked. Its absence let every callers
    family argue its mechanism on reference-noise (grep hits per REFERENCE)
    while grading against CALL SITES -- which put a 45x cell (LastMessage) in
    the 'easy' callers control and argued a parity call from the most
    favourable of six cells.
    """

    @classmethod
    def setUpClass(cls):
        cls.reg = _load(REGISTRY_PATH)
        cls.families = cls.reg["families"]
        cls.cohorts = _load(COHORTS_PATH)
        cls.by_slug = {s["slug"]: s for s in cls.cohorts["symbols"]}

    def test_every_family_is_cohorted_on_the_axis_its_answer_set_implies(self):
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                self.assertIsNone(axis_violation(fam))

    def test_declared_axes_are_defined_in_the_registrys_axis_block(self):
        axes = self.reg["difficulty_axes"]
        self.assertIn("call_site_noise", axes)
        self.assertIn("reference_noise", axes)
        self.assertEqual(axes["call_site_noise"]["formula"], "grep_hit_count / fan_in")
        self.assertEqual(
            axes["reference_noise"]["formula"], "grep_hit_count / true_reference_count"
        )

    def test_call_site_families_derive_from_d2s_own_published_thresholds(self):
        # Re-deriving with thresholds chosen here rather than inherited would
        # make the cell boundaries a tuning knob.
        th = self.cohorts["thresholds"]
        derived = self.reg["difficulty_axes"]["derivation_thresholds"]
        self.assertEqual(derived["call_site_control_max"], th["control_max_noise"])
        self.assertEqual(derived["call_site_high_min"], th["high_min_noise"])

    def test_call_site_family_members_all_satisfy_their_stated_predicate(self):
        for fam in self.families:
            sel = fam["cohort"]["selector"]
            if sel["kind"] != "derived_call_site_noise":
                continue
            with self.subTest(family=fam["id"]):
                members = derive_members(sel, self.cohorts)
                self.assertTrue(members, f"{fam['id']} derives an empty cell")
                for slug in members:
                    ratio = call_site_noise(self.by_slug[slug])
                    if sel["operator"] == ">=":
                        self.assertGreaterEqual(ratio, sel["value"])
                    else:
                        self.assertLessEqual(ratio, sel["value"])

    def test_no_callers_family_contains_a_zero_caller_cell(self):
        # call_site_noise is undefined at fan_in=0, and an empty answer set
        # makes F1 undefined. Their exclusion must follow from the stated
        # fan_in>=1 rule, not from taste.
        for fam in self.families:
            if fam["task_shape"] != "exhaustive_callers":
                continue
            with self.subTest(family=fam["id"]):
                members = derive_members(fam["cohort"]["selector"], self.cohorts)
                zero = [s for s in members if self.by_slug[s]["fan_in"] == 0]
                self.assertEqual(zero, [], f"{fam['id']} contains zero-caller cells")

    def test_every_zero_caller_cell_is_named_in_the_binding_d4_finding(self):
        # Five cells, not one. D4's empty-answer grading rule must cover them
        # all, so the finding has to enumerate them.
        zero = [s for s in self.cohorts["symbols"] if s["fan_in"] == 0]
        self.assertEqual(len(zero), 5, "sample drifted from 5 zero-caller cells")
        finding = self.reg["registry_findings"]["zero_caller_cells"]
        for sym in zero:
            self.assertIn(
                sym["name"], finding, f"{sym['name']} missing from zero_caller_cells"
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
                self.assertIn(pred["outcome"], VALID_OUTCOMES)
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
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                self.assertIn(fam["power"]["status"], VALID_POWER_STATUS)
                self.assertIsNone(threshold_marker_violation(fam))

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

    def test_every_metric_declares_its_polarity(self):
        for name, spec in self.reg["primary_metrics"].items():
            with self.subTest(metric=name):
                self.assertIn(spec["polarity"], {"higher_is_better", "lower_is_better"})
                self.assertTrue(spec["description"].strip())

    def test_parity_predictions_use_a_two_sided_band(self):
        # A parity prediction is only falsifiable if a win in EITHER direction
        # breaks it. A one-sided parity threshold could not be contradicted by
        # an LCI win, which would make it a hedge.
        for fam in self.families:
            if fam["prediction"]["outcome"] != "parity":
                continue
            with self.subTest(family=fam["id"]):
                self.assertEqual(
                    fam["threshold"]["comparison"], "abs_treatment_minus_baseline"
                )
                self.assertEqual(fam["threshold"]["operator"], "<=")

    def test_directional_predictions_are_signed_toward_the_predicted_winner(self):
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
                self.assertGreater(thr["value"], 0)

    def test_matched_control_shares_the_metric_it_controls_for(self):
        # A control on a different metric would not be a null check for the
        # comparison it claims to guard.
        by_id = {f["id"]: f for f in self.families}
        for fam in self.families:
            counterpart = fam.get("control_counterpart")
            if not counterpart or fam["threshold"] is None:
                continue
            with self.subTest(family=fam["id"]):
                self.assertEqual(
                    fam["threshold"]["metric"],
                    by_id[counterpart]["threshold"]["metric"],
                    f"{fam['id']} and control {counterpart} must be read on one metric",
                )

    def test_confidence_is_a_probability_or_null_only_when_untestable(self):
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                conf = fam["prediction"]["confidence"]
                if conf is None:
                    self.assertEqual(fam["power"]["status"], "untestable_at_this_n")
                else:
                    self.assertGreater(conf, 0.0)
                    self.assertLess(conf, 1.0)

    def test_epic_intuition_is_recorded_so_d5_can_score_the_intuition_too(self):
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                self.assertTrue(fam["epic_intuition"].strip())

    def test_families_contradicting_the_epic_intuition_say_so_explicitly(self):
        for fam in self.families:
            intuition = fam["epic_intuition"]
            outcome = fam["prediction"]["outcome"]
            if intuition in VALID_OUTCOMES and intuition != outcome:
                with self.subTest(family=fam["id"]):
                    self.assertIn(
                        "CONTRADICT",
                        fam["prediction"].get("note", "").upper(),
                        f"{fam['id']} predicts {outcome} against an intuition of "
                        f"{intuition} without flagging the contradiction",
                    )


class PowerHonestyTest(unittest.TestCase):
    """Power status is re-derived from the committed sample, never trusted."""

    @classmethod
    def setUpClass(cls):
        cls.reg = _load(REGISTRY_PATH)
        cls.families = cls.reg["families"]
        cls.cohorts = _load(COHORTS_PATH)

    def _derive_n(self, fam):
        return len(derive_members(fam["cohort"]["selector"], self.cohorts))

    def test_declared_n_matches_the_committed_cohort_sample(self):
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                self.assertEqual(
                    fam["cohort"]["n"],
                    self._derive_n(fam),
                    f"{fam['id']} declares an n the committed cohort file does not "
                    "support -- a wrong n would silently justify a wrong power verdict",
                )

    def test_power_status_follows_from_the_derived_n(self):
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                n = self._derive_n(fam)
                self.assertEqual(
                    fam["power"]["status"],
                    power_status_for(n, fam["power"]["min_n_for_80pct_power"]),
                    f"{fam['id']} n={n}",
                )

    def test_required_n_is_not_understated_for_the_familys_metric(self):
        for fam in self.families:
            thr = fam["threshold"]
            if thr is None:
                continue
            with self.subTest(family=fam["id"]):
                self.assertGreaterEqual(
                    fam["power"]["min_n_for_80pct_power"],
                    MIN_N_FLOOR_BY_METRIC[thr["metric"]],
                    f"{fam['id']} understates what {thr['metric']} needs",
                )

    def test_every_family_records_n_and_the_power_bars(self):
        for fam in self.families:
            with self.subTest(family=fam["id"]):
                self.assertEqual(fam["power"]["n"], fam["cohort"]["n"])
                self.assertIsInstance(fam["power"]["min_n_for_80pct_power"], int)
                self.assertTrue(
                    fam["power"]["note"].strip(),
                    f"{fam['id']} must state its power situation in prose D5 can quote",
                )

    def test_underpowered_families_state_the_n_they_would_need(self):
        for fam in self.families:
            if fam["power"]["status"] == "powered":
                continue
            with self.subTest(family=fam["id"]):
                self.assertGreater(
                    fam["power"]["min_n_for_80pct_power"], fam["power"]["n"]
                )

    def test_untestable_families_are_retained_not_dropped(self):
        untestable = [
            f for f in self.families if f["power"]["status"] == "untestable_at_this_n"
        ]
        self.assertTrue(
            untestable,
            "impl_many (n=2) is in the sample; a registry that dropped it would hide "
            "the gap the epic's power warning requires be recorded",
        )
        for fam in untestable:
            with self.subTest(family=fam["id"]):
                self.assertIsNone(fam["threshold"])
                self.assertIn("UNTESTABLE", fam["power"]["note"].upper())

    def test_n_equals_two_family_is_marked_untestable(self):
        self.assertEqual(len(self.cohorts["cohorts"]["impl_many"]), 2)
        impl = [f for f in self.families if f["cohort"]["cell"] == "impl_many"]
        self.assertEqual(len(impl), 1, "the impl_many family must be on the record")
        self.assertEqual(impl[0]["power"]["status"], "untestable_at_this_n")
        self.assertIsNone(impl[0]["threshold"])

    def test_rare_event_dnf_family_is_not_certified_powered_by_cell_count_alone(self):
        # token_budgeted_completion has the LARGEST n here (48, the whole pool)
        # and passes any global n>=25 bar, yet cannot detect the 4.3pp gap its
        # own prior predicts. If this reports "powered", the power rule has
        # silently reverted to a global cell-count bar.
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


class ControlCoverageTest(unittest.TestCase):
    """The instrument must be able to report a null, and be shown to."""

    @classmethod
    def setUpClass(cls):
        cls.reg = _load(REGISTRY_PATH)
        cls.families = cls.reg["families"]
        cls.by_id = {f["id"]: f for f in cls.families}

    def test_registry_passes_the_control_gate(self):
        self.assertIsNone(control_gate_violation(self.families))

    def test_controls_are_powered_enough_to_do_their_job(self):
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
                self.assertTrue(fam["controls_for"])
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
                    self.assertGreater(
                        len(fam.get("control_counterpart_rationale", "")),
                        80,
                        f"{fam['id']} has no control counterpart and no substantive "
                        "rationale for its absence",
                    )
                else:
                    self.assertIn(counterpart, self.by_id)
                    self.assertEqual(self.by_id[counterpart]["role"], "control")

    def test_matched_controls_share_the_task_shape_they_control_for(self):
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

    def test_matched_callers_control_sits_on_the_same_axis_as_its_treatment(self):
        # The control only isolates collision noise if it is the EASY END of the
        # SAME axis. A control drawn from a different axis would vary two things.
        high = self.by_id["callers_call_site_high"]
        low = self.by_id["control_callers_call_site_control"]
        self.assertEqual(high["cohort"]["axis"], low["cohort"]["axis"])
        self.assertEqual(high["cohort"]["selector"]["operator"], ">=")
        self.assertEqual(low["cohort"]["selector"]["operator"], "<=")
        self.assertGreater(
            high["cohort"]["selector"]["value"], low["cohort"]["selector"]["value"]
        )

    def test_shared_cell_families_declare_their_non_independence(self):
        for fam in self.families:
            for other in fam.get("shares_cells_with", []):
                with self.subTest(family=fam["id"]):
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
        policy = _load(REGISTRY_PATH)["amendment_policy"]
        self.assertIn("MUST NOT be edited", policy)
        self.assertIn("void", policy.lower())

    def test_every_family_carries_a_void_flag_for_the_amendment_path(self):
        for fam in _load(REGISTRY_PATH)["families"]:
            with self.subTest(family=fam["id"]):
                self.assertIn("voided", fam)
                self.assertIsInstance(fam["voided"], bool)

    def test_retired_families_are_recorded_not_silently_dropped(self):
        findings = _load(REGISTRY_PATH)["registry_findings"]
        self.assertIn("families_considered_and_not_registered", findings)
        self.assertIn("callers_high_fan_in", findings["families_considered_and_not_registered"])


class DiscriminationTest(unittest.TestCase):
    """Prove the gates FAIL on the bad registries they exist to catch.

    Every case is a defect that actually occurred while writing this registry,
    or the specific hedge the acceptance criteria forbid. A gate only ever
    observed passing is unverified, so each calls the SAME function the live
    assertions call -- never a re-implementation, which would only prove a copy
    of the rule works.
    """

    def _real(self):
        return _load(REGISTRY_PATH)

    # --- axis grounding: THE defect that shipped to review -----------------

    def test_axis_gate_rejects_a_callers_family_cohorted_on_reference_noise(self):
        # THE ACTUAL DEFECT. schema_version 1 cohorted every callers family on
        # reference-noise while grading call sites. This gate is what makes that
        # unshippable.
        reg = self._real()
        fam = copy.deepcopy(
            next(f for f in reg["families"] if f["id"] == "callers_call_site_high")
        )
        self.assertIsNone(axis_violation(fam))
        fam["cohort"]["axis"] = "noise"  # what version 1 said
        violation = axis_violation(fam)
        self.assertIsNotNone(violation, "reference-noise on a callers family must fail")
        self.assertIn("call_site_noise", violation)

    def test_the_two_noise_axes_actually_disagree_on_a_real_cell(self):
        # The gate above only matters if the axes diverge in practice. They do:
        # LastMessage is QUIET per reference (0.94, inside D2's noise_control)
        # yet returns 45 grep hits for its single call site. A reference-noise
        # control would have imported a 45x cell into the "easy" callers family.
        cohorts = _load(COHORTS_PATH)
        by_slug = {s["slug"]: s for s in cohorts["symbols"]}
        last = next(s for s in cohorts["symbols"] if s["name"] == "LastMessage")
        self.assertLessEqual(reference_noise(last), 2.0)  # looks like a control cell
        self.assertIn(last["slug"], cohorts["cohorts"]["noise_control"])  # and IS one
        self.assertGreaterEqual(call_site_noise(last), 45.0)  # but is 45x for callers

        reg = self._real()
        low = next(
            f for f in reg["families"] if f["id"] == "control_callers_call_site_control"
        )
        members = derive_members(low["cohort"]["selector"], cohorts)
        self.assertNotIn(
            last["slug"], members, "the corrected control must exclude LastMessage"
        )
        high = next(f for f in reg["families"] if f["id"] == "callers_call_site_high")
        self.assertIn(
            last["slug"],
            derive_members(high["cohort"]["selector"], cohorts),
            "and must classify it as high call-site noise",
        )
        self.assertTrue(by_slug[last["slug"]]["fan_in"] == 1)

    def test_call_site_noise_refuses_the_zero_caller_cells_rather_than_defaulting(self):
        # Fail-closed: an undefined ratio must raise, not silently become 0 or
        # inf and land the cell in a cohort by accident.
        cohorts = _load(COHORTS_PATH)
        send = next(s for s in cohorts["symbols"] if s["name"] == "send")
        self.assertEqual(send["fan_in"], 0)
        with self.assertRaises(ZeroDivisionError):
            call_site_noise(send)

    def test_axis_gate_rejects_a_confounded_family_that_hides_its_confound(self):
        reg = self._real()
        fam = copy.deepcopy(
            next(f for f in reg["families"] if f["id"] == "transitive_callers_depth2")
        )
        self.assertIsNone(axis_violation(fam))
        fam["prediction"]["note"] = "no caveat here at all"
        self.assertIsNotNone(axis_violation(fam))

    # --- metric polarity ---------------------------------------------------

    def test_polarity_rule_rejects_dnf_signed_as_if_higher_were_better(self):
        # ACTUAL DEFECT: signing an lci_wins DNF prediction as
        # treatment_minus_baseline predicts LCI does MORE not-finishing.
        self.assertEqual(
            expected_comparison("lci_wins", "lower_is_better"), "baseline_minus_treatment"
        )
        self.assertNotEqual(
            expected_comparison("lci_wins", "lower_is_better"), "treatment_minus_baseline"
        )

    def test_polarity_rule_separates_the_two_polarities_in_both_directions(self):
        self.assertEqual(
            expected_comparison("lci_wins", "higher_is_better"), "treatment_minus_baseline"
        )
        self.assertEqual(
            expected_comparison("grep_wins", "higher_is_better"), "baseline_minus_treatment"
        )
        self.assertEqual(
            expected_comparison("grep_wins", "lower_is_better"), "treatment_minus_baseline"
        )

    # --- power -------------------------------------------------------------

    def test_power_rule_refuses_to_certify_the_rare_event_family_at_n48(self):
        # ACTUAL DEFECT: a global n>=25 bar certified n=48 as powered when the
        # DNF metric needs ~900.
        self.assertEqual(power_status_for(48, 900), "underpowered")

    def test_power_rule_certifies_only_when_n_meets_the_metrics_own_bar(self):
        self.assertEqual(power_status_for(26, 25), "powered")
        self.assertEqual(power_status_for(24, 25), "underpowered")
        self.assertEqual(power_status_for(13, 25), "underpowered")

    def test_power_rule_calls_any_n_below_six_untestable_however_low_the_bar(self):
        # Below n=6 no split reaches p<0.05, so no declared requirement can
        # rescue the family -- including n=5, which a "n<=2" rule would have
        # wrongly let through with a threshold.
        for n in (2, 3, 4, 5):
            self.assertEqual(power_status_for(n, 1), "untestable_at_this_n", f"n={n}")
        self.assertEqual(power_status_for(6, 25), "underpowered")

    def test_min_n_floor_rejects_a_family_understating_what_its_metric_needs(self):
        # Self-certification path: declare min_n=6 on an F1 family with n=6 and
        # the status rule alone would happily call it powered.
        self.assertEqual(power_status_for(6, 6), "powered")
        self.assertLess(6, MIN_N_FLOOR_BY_METRIC["mean_f1"])  # the floor catches it

    # --- registry-level gates ----------------------------------------------

    def test_threshold_gate_rejects_an_untestable_family_wearing_a_threshold(self):
        # The forbidden hedge: dressing an n=2 family up with a numeric
        # threshold its sample can never test.
        reg = self._real()
        fam = copy.deepcopy(
            next(f for f in reg["families"] if f["power"]["status"] == "untestable_at_this_n")
        )
        self.assertIsNone(threshold_marker_violation(fam))
        fam["threshold"] = {
            "metric": "mean_f1",
            "comparison": "treatment_minus_baseline",
            "operator": ">=",
            "value": 0.20,
            "falsified_if": "looks rigorous, tests nothing",
        }
        self.assertIsNotNone(threshold_marker_violation(fam))

    def test_threshold_gate_rejects_a_missing_threshold_without_the_marker(self):
        reg = self._real()
        fam = copy.deepcopy(next(f for f in reg["families"] if f["threshold"] is not None))
        self.assertIsNone(threshold_marker_violation(fam))
        fam["threshold"] = None
        self.assertIsNotNone(threshold_marker_violation(fam))

    def test_control_gate_rejects_a_registry_with_only_lci_win_predictions(self):
        # The rigged sweep the criterion exists to prevent.
        reg = self._real()
        self.assertIsNone(control_gate_violation(reg["families"]))
        rigged = copy.deepcopy(reg["families"])
        for fam in rigged:
            fam["role"] = "hypothesis"
            fam["prediction"]["outcome"] = "lci_wins"
        self.assertIsNotNone(control_gate_violation(rigged))

    def test_control_gate_rejects_a_registry_that_cannot_report_a_grep_win(self):
        # Parity controls alone only prove the instrument can report "no
        # difference", not that it can report an LCI LOSS.
        reg = self._real()
        no_grep = copy.deepcopy(reg["families"])
        for fam in no_grep:
            if fam["prediction"]["outcome"] == "grep_wins":
                fam["prediction"]["outcome"] = "parity"
        violation = control_gate_violation(no_grep)
        self.assertIsNotNone(violation)
        self.assertIn("grep_wins", violation)

    def test_declared_n_gate_rejects_an_n_the_cohort_file_does_not_support(self):
        # Inflating n is how an underpowered family would launder itself into a
        # powered verdict.
        cohorts = _load(COHORTS_PATH)
        real_n = len(cohorts["cohorts"]["impl_many"])
        self.assertEqual(power_status_for(30, 25), "powered")  # the lie would work
        self.assertEqual(power_status_for(real_n, 25), "untestable_at_this_n")


if __name__ == "__main__":
    unittest.main()
