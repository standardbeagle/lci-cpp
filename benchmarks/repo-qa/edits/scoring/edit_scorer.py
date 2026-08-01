"""Score Stage-3 edit runs by THREE gates, then aggregate the two arms.

The headline metric is the ALL-THREE-GATES pass rate per arm: a patch counts
only when behaviour AND convention AND blast-radius all pass. Reporting a
behaviour-only pass rate would flatter every arm -- making the reference tests
green is the easy third of the job; matching the repo's conventions and not
rippling past the declared blast radius is the rest.

The three gates are DERIVED from the two gate outcomes the S3.4 runner persists,
consuming all four oracle sub-outcomes (none may be silently dropped):

    behavior   = oracle.existing_suite AND oracle.discrimination
                 (the task's red->green proof, plus the repo's existing suite as
                 a regression guard -- a green task test must not mask a red
                 suite)
    convention = conformance.passed
    blast      = oracle.changed_path AND oracle.api_impact
                 (the declared path scope, plus the call-hierarchy ripple -- an
                 in-scope diff that escapes the declared API is still a
                 blast-radius failure)

Two fail-loud rules run through everything here (karpathy rule 6):

  * NOTHING MISSING BECOMES A SUCCESS. The headline denominator is the PLANNED
    cell set (task x arm x repetition), not the observed one, so a cell that
    never ran cannot raise a rate by leaving the denominator. Absent cells are
    listed and fail completeness.
  * AN ABSENT GATE IS A FAILURE, not an unknown. A record whose gates were never
    filled in scores false on all three, with reason GATE_ABSENT.

Failure buckets stay distinct rather than collapsing into one "failed" count,
because they support opposite conclusions: only ``patch_rejected`` is evidence
the agent was wrong. ``harness_failure`` and ``oracle_failure`` are evidence WE
are wrong, and ``agent_failure`` / ``invalid_patch`` say the attempt never
reached judgement.

Everything is deterministic: reason codes and cells are sorted before emit and
no wall-clock, RNG or hash iteration order reaches the output.
"""

import itertools
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))          # edits/scoring
_EDITS = os.path.dirname(_HERE)                              # edits

# _HERE first so sibling scoring modules import as top-level names; edit_paths
# (the runner's bootstrap) is the ONE place the sibling package wiring lives.
for _p in (_HERE, os.path.join(_EDITS, "runner")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import edit_paths  # noqa: F401,E402  (puts exploration/ + the gates on the path)
import jsonschema  # noqa: E402

import edit_record  # noqa: E402

# One source of truth with the exploration scorer for the process metrics and
# the compatibility guard -- these are the same computations over the same
# record fields, so they are reused rather than re-derived (cf. edit_record.py
# reusing the exploration record log verbatim).
from scoring.scorer import (  # noqa: E402
    IncompatibleRuns,
    _check_compatibility,
    _delta,
    _distribution,
    _pair_scores,
    _process,
)

SCORE_SCHEMA = "edit_score_v1"
AGGREGATE_SCHEMA = "edit_aggregate_v1"

SCORE_SCHEMA_PATH = os.path.join(_HERE, "edit-score.schema.json")
AGGREGATE_SCHEMA_PATH = os.path.join(_HERE, "edit-aggregate.schema.json")

GATE_BEHAVIOR = "behavior"
GATE_CONVENTION = "convention"
GATE_BLAST = "blast"
# Matrix bit order is this tuple's order.
GATE_NAMES = (GATE_BEHAVIOR, GATE_CONVENTION, GATE_BLAST)

# The oracle sub-outcomes each derived gate consumes. Every sub-outcome the
# oracle schema defines appears exactly once: dropping one would silently narrow
# the gate.
_ORACLE_SUBGATES = {
    GATE_BEHAVIOR: ("existing_suite", "discrimination"),
    GATE_BLAST: ("changed_path", "api_impact"),
}

REASON_GATE_ABSENT = "GATE_ABSENT"

# --- failure buckets (criterion 4: never collapsed) ---
FAILURE_AGENT = "agent_failure"
FAILURE_INVALID_PATCH = "invalid_patch"
FAILURE_HARNESS = "harness_failure"
FAILURE_ORACLE = "oracle_failure"
FAILURE_PATCH_REJECTED = "patch_rejected"
FAILURE_MISSING_CELL = "missing_cell"

FAILURE_CLASSES = (
    FAILURE_AGENT,
    FAILURE_INVALID_PATCH,
    FAILURE_HARNESS,
    FAILURE_ORACLE,
    FAILURE_PATCH_REJECTED,
    FAILURE_MISSING_CELL,
)

# The runner's terminal statuses that decide the bucket on their own, before any
# gate reason is consulted: these attempts never reached a verdict.
_STATUS_FAILURE = {
    edit_record.STATUS_AGENT_FAILURE: FAILURE_AGENT,
    edit_record.STATUS_TIMEOUT: FAILURE_AGENT,
    edit_record.STATUS_TOOL_VIOLATION: FAILURE_AGENT,
    edit_record.STATUS_INVALID_PATCH: FAILURE_INVALID_PATCH,
    edit_record.STATUS_CONFIG_ERROR: FAILURE_HARNESS,
}

# Our tooling or environment broke -- says nothing about the agent's patch.
_HARNESS_REASONS = frozenset(
    {
        "RUN_ERROR",
        "TIMEOUT",
        "COMMAND_NOT_FOUND",
        "COMMAND_ABSENT",
        "TOOL_FAILURE",
        "MANIFEST_ABSENT",
        "ORACLE_PATCH_ABSENT",
        "LCI_ABSENT",
        "BLAST_RADIUS_MALFORMED",
        "RULE_MALFORMED",
        "RULE_UNSUPPORTED",
        REASON_GATE_ABSENT,
        "REASON_UNCLASSIFIED",
    }
)

# The gate ran, but the oracle/anchor material it judged with is itself
# defective -- the cell yields no evidence either way about the patch.
_ORACLE_REASONS = frozenset(
    {
        "NON_DISCRIMINATING",
        "WRONG_DIRECTION",
        "ANCHOR_MALFORMED",
        "ANCHOR_DECOY",
        "ANCHOR_NOT_LIVE",
        "ANCHOR_MISSING",
        "ANCHOR_BOUND_STALE",
        "ANCHOR_IDENTIFIER_ABSENT",
        "MATCH_AMBIGUOUS",
    }
)

# The gates judged the patch and it was genuinely wrong -- the only reasons that
# are evidence about the AGENT.
_PATCH_REASONS = frozenset(
    {
        "SUITE_RED",
        "PATH_OUT_OF_SCOPE",
        "TOO_MANY_FILES",
        "NO_CHANGES",
        "API_IMPACT_ESCAPE",
        "PATCH_PATH_ESCAPE",
        "MATCH_ABSENT",
    }
)

_PASSING_REASONS = frozenset(
    {"SUITE_GREEN", "DISCRIMINATES", "WITHIN_SCOPE", "NO_ESCAPE", "CONFORMS"}
)

# A code we cannot place means OUR mapping is stale, so it classifies as a
# harness failure rather than being charged to the agent by default.
REASON_UNCLASSIFIED = "REASON_UNCLASSIFIED"

_KNOWN_REASONS = (
    _HARNESS_REASONS | _ORACLE_REASONS | _PATCH_REASONS | _PASSING_REASONS
)


# ---------------------------------------------------------------------------
# schema validation (criterion 1)
# ---------------------------------------------------------------------------


def _load_schema(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def validate_score(score):
    """Raise jsonschema.ValidationError unless `score` is a valid edit_score_v1."""
    jsonschema.Draft202012Validator(_load_schema(SCORE_SCHEMA_PATH)).validate(score)
    return score


def validate_aggregate(report):
    """Raise jsonschema.ValidationError unless `report` is a valid
    edit_aggregate_v1."""
    jsonschema.Draft202012Validator(_load_schema(AGGREGATE_SCHEMA_PATH)).validate(
        report
    )
    return report


# ---------------------------------------------------------------------------
# per-attempt scoring
# ---------------------------------------------------------------------------


def _absent_gate():
    """Fail-closed gate: unjudged is a FAIL, never an unknown or a pass."""
    return {"passed": False, "judged": False, "reasons": [REASON_GATE_ABSENT]}


def _derive_gate(subs):
    """Conjoin sub-outcomes into one gate; a missing sub-outcome fails closed."""
    if any(sub is None for sub in subs):
        return _absent_gate()
    passed = all(bool(sub.get("passed")) for sub in subs)
    reasons = {sub.get("reason") for sub in subs if sub.get("reason")}
    if passed:
        # Keep the passing codes out of the failure story; they carry no signal.
        reasons -= _PASSING_REASONS
    return {
        "passed": passed,
        "judged": True,
        "reasons": sorted(r for r in reasons if r),
    }


def _reason_from_diagnostic(diagnostic):
    """The leading reason code of a gate diagnostic.

    A rule-level conformance failure (malformed/unsupported rule, absent
    manifest) emits NO anchors, so its reason code exists only as the
    diagnostic's documented leading token. An unrecognised head is reported as
    REASON_UNCLASSIFIED rather than guessed at.
    """
    head = (diagnostic or "").split(":", 1)[0].strip()
    return head if head in _KNOWN_REASONS else REASON_UNCLASSIFIED


def _derive_conformance(outcome):
    """The convention gate. Unlike the oracle's sub-outcomes, a conformance
    verdict carries its reason codes per-anchor, with rule-level failures
    falling back to the diagnostic."""
    if outcome is None:
        return _absent_gate()
    if outcome.get("passed"):
        return {"passed": True, "judged": True, "reasons": []}
    reasons = {
        anchor.get("reason")
        for anchor in (outcome.get("anchors") or [])
        if not anchor.get("passed") and anchor.get("reason")
    }
    if not reasons:
        reasons = {_reason_from_diagnostic(outcome.get("diagnostic"))}
    return {"passed": False, "judged": True, "reasons": sorted(reasons)}


def _gates_of(record):
    """The three derived gates for one run record, fail-closed throughout."""
    raw = record.get("gates") or {}
    oracle = raw.get("oracle")
    conformance = raw.get("conformance")

    derived = {}
    for gate, sub_names in _ORACLE_SUBGATES.items():
        if oracle is None:
            derived[gate] = _absent_gate()
        else:
            derived[gate] = _derive_gate([oracle.get(name) for name in sub_names])
    derived[GATE_CONVENTION] = _derive_conformance(conformance)
    return derived


def _matrix_cell(derived):
    """This attempt's eight-cell matrix key, ordered behavior, convention, blast."""
    return "".join("T" if derived[name]["passed"] else "F" for name in GATE_NAMES)


def _classify_failure(record, derived):
    """The ONE bucket a non-passing attempt belongs to.

    Status decides first (those attempts never reached a verdict), then gate
    reasons, worst-provenance-first: a harness break outranks a defective
    oracle, which outranks blaming the patch. Only the last bucket is evidence
    the agent was wrong.
    """
    status = record.get("status")
    if status in _STATUS_FAILURE:
        return _STATUS_FAILURE[status]

    reasons = {r for gate in derived.values() for r in gate["reasons"]}
    if (reasons & _HARNESS_REASONS) or (reasons - _KNOWN_REASONS):
        return FAILURE_HARNESS
    if reasons & _ORACLE_REASONS:
        return FAILURE_ORACLE
    return FAILURE_PATCH_REJECTED


def score_run(record):
    """Score one S3.4 edit run record. Returns a schema-versioned score dict."""
    derived = _gates_of(record)
    all_passed = all(derived[name]["passed"] for name in GATE_NAMES)
    return {
        "schema": SCORE_SCHEMA,
        "run_key": record.get("run_key"),
        "task_id": record.get("task_id"),
        "arm": record.get("arm"),
        "seed": record.get("seed"),
        "cell": {
            "task_id": record.get("task_id"),
            "arm": record.get("arm"),
            "seed": record.get("seed"),
        },
        "corpus_id": record.get("corpus_id"),
        "model": record.get("model"),
        "forge_version": record.get("forge_version"),
        "task_bank_version": record.get("task_schema"),
        "task_digest": record.get("task_digest"),
        "patch_hash": record.get("patch_hash"),
        "status": record.get("status"),
        "reason": record.get("reason"),
        "gates": derived,
        "all_gates_passed": all_passed,
        "matrix_cell": _matrix_cell(derived),
        "failure_class": None if all_passed else _classify_failure(record, derived),
        "process": _process(record),
    }


# ---------------------------------------------------------------------------
# the planned cell set -- the headline denominator (criterion 2)
# ---------------------------------------------------------------------------

# Every matrix key, in behavior/convention/blast bit order. Materialised so the
# matrix is always zero-filled and byte-stable rather than shaped by whichever
# outcomes a given run happened to produce.
MATRIX_CELLS = tuple("".join(bits) for bits in itertools.product("TF", repeat=3))


def _cell_key(cell):
    """Total, type-stable sort key for a cell (seeds stay numerically ordered)."""
    seed = cell["seed"]
    return (
        str(cell["task_id"]),
        str(cell["arm"]),
        seed is None,
        seed if isinstance(seed, int) else 0,
        str(seed),
    )


def _cell_id(cell):
    return (cell["task_id"], cell["arm"], cell["seed"])


def planned_cells(task_ids, arms, seeds):
    """The full task x arm x repetition grid the run was SUPPOSED to cover.

    This -- not the set of records that came back -- is the headline
    denominator, so a cell that crashed or never ran cannot raise a rate by
    quietly leaving the divisor.
    """
    return sorted(
        (
            {"task_id": task_id, "arm": arm, "seed": seed}
            for task_id in task_ids
            for arm in arms
            for seed in seeds
        ),
        key=_cell_key,
    )


# ---------------------------------------------------------------------------
# aggregation
# ---------------------------------------------------------------------------


def _latest_by_cell(scores):
    """Last score wins per cell, matching the append-log's resume semantics."""
    latest = {}
    for score in scores:
        latest[_cell_id(score["cell"])] = score
    return latest


def _gate_rates(scores, planned_n):
    """Per-gate counts and rates, over the PLANNED denominator like the
    headline -- an unjudged or missing cell counts against every gate."""
    rates = {}
    for name in GATE_NAMES:
        passed = sum(1 for s in scores if s["gates"][name]["passed"])
        rates[name] = {
            "passed": passed,
            "judged": sum(1 for s in scores if s["gates"][name]["judged"]),
            "rate": (passed / planned_n) if planned_n else None,
        }
    return rates


def _matrix(scores):
    """The eight-cell behaviour x convention x blast pass matrix, zero-filled."""
    matrix = {cell: 0 for cell in MATRIX_CELLS}
    for score in scores:
        matrix[score["matrix_cell"]] += 1
    return matrix


def _failure_reasons(scores, missing_n):
    """The failure breakdown over planned cells, every bucket zero-filled.

    Absent cells are a bucket of their own rather than a silent shortfall, so
    all_pass + sum(buckets) == planned holds and nothing can go missing without
    showing up here.
    """
    buckets = {name: 0 for name in FAILURE_CLASSES}
    for score in scores:
        if not score["all_gates_passed"]:
            buckets[score["failure_class"]] += 1
    buckets[FAILURE_MISSING_CELL] += missing_n
    return buckets


def _arm_summary(scores, planned_n, missing_n):
    return {
        "planned": planned_n,
        "observed": len(scores),
        "all_pass": sum(1 for s in scores if s["all_gates_passed"]),
        "gates": _gate_rates(scores, planned_n),
        "matrix": _matrix(scores),
        "failure_reasons": _failure_reasons(scores, missing_n),
        "tool_calls": _distribution([s["process"]["tool_calls"] for s in scores]),
        "input_tokens": _distribution([s["process"]["input_tokens"] for s in scores]),
        "output_tokens": _distribution([s["process"]["output_tokens"] for s in scores]),
        "wall_clock_seconds": _distribution(
            [s["process"]["wall_clock_seconds"] for s in scores]
        ),
    }


def _headline_arm(summary):
    planned_n = summary["planned"]
    return {
        "planned": planned_n,
        "observed": summary["observed"],
        "all_pass": summary["all_pass"],
        "rate": (summary["all_pass"] / planned_n) if planned_n else None,
    }


def _deltas(paired_arms):
    """Treatment-minus-baseline over PAIRED cells; {} when an arm is absent."""
    treatment = paired_arms.get("treatment")
    baseline = paired_arms.get("baseline")
    if treatment is None or baseline is None:
        return {}

    def _mean(summary, field):
        dist = summary[field]
        return dist["mean"] if dist else None

    deltas = {
        "all_three_gates_pass_rate": _delta(
            _headline_arm(treatment)["rate"], _headline_arm(baseline)["rate"]
        )
    }
    for name in GATE_NAMES:
        deltas[f"{name}_rate"] = _delta(
            treatment["gates"][name]["rate"], baseline["gates"][name]["rate"]
        )
    for field, key in (
        ("tool_calls", "tool_calls_mean"),
        ("input_tokens", "input_tokens_mean"),
        ("output_tokens", "output_tokens_mean"),
        ("wall_clock_seconds", "wall_clock_mean"),
    ):
        deltas[key] = _delta(_mean(treatment, field), _mean(baseline, field))
    return deltas


def aggregate(score_records, planned, *, group_by=()):
    """Aggregate scored edit attempts against the PLANNED cell set.

    `planned` is the task x arm x repetition grid from `planned_cells`. It is
    the denominator for the headline and for every gate rate: absent cells are
    enumerated under `completeness`, fail it, and are bucketed as missing_cell
    -- they never leave the divisor.

    Raises IncompatibleRuns if the records disagree on model, forge version, or
    task-bank version unless that field is named in `group_by`.
    """
    observed = _latest_by_cell(list(score_records))
    planned = sorted(planned, key=_cell_key)
    planned_ids = {_cell_id(cell) for cell in planned}

    scores = list(observed.values())
    shared = _check_compatibility(scores, group_by) if scores else {}

    missing = [cell for cell in planned if _cell_id(cell) not in observed]
    unplanned = sorted(
        (dict(score["cell"]) for cid, score in observed.items() if cid not in planned_ids),
        key=_cell_key,
    )

    # Only PLANNED cells feed the rates; an unplanned stray is reported under
    # completeness rather than folded into a denominator it was never part of.
    by_arm = {}
    for cid, score in observed.items():
        if cid in planned_ids:
            by_arm.setdefault(score["arm"], []).append(score)

    planned_by_arm = {}
    missing_by_arm = {}
    for cell in planned:
        planned_by_arm[cell["arm"]] = planned_by_arm.get(cell["arm"], 0) + 1
    for cell in missing:
        missing_by_arm[cell["arm"]] = missing_by_arm.get(cell["arm"], 0) + 1

    arms = {}
    for arm in sorted(set(planned_by_arm) | set(by_arm)):
        arm_scores = sorted(by_arm.get(arm, []), key=lambda s: _cell_key(s["cell"]))
        arms[arm] = _arm_summary(
            arm_scores, planned_by_arm.get(arm, 0), missing_by_arm.get(arm, 0)
        )

    paired_scores, unpaired = _pair_scores(
        [s for group in by_arm.values() for s in group]
    )
    paired_by_arm = {}
    for score in paired_scores:
        paired_by_arm.setdefault(score["arm"], []).append(score)
    paired_arms = {
        arm: _arm_summary(paired_by_arm[arm], len(paired_by_arm[arm]), 0)
        for arm in sorted(paired_by_arm)
    }

    complete = not missing and not unplanned
    return {
        "schema": AGGREGATE_SCHEMA,
        "grouped_by": sorted(group_by or ()),
        "compatibility": shared,
        "headline": {
            "metric": "all_three_gates_pass_rate",
            "definition": (
                "share of PLANNED task x arm x repetition cells whose patch "
                "passed behaviour AND convention AND blast-radius"
            ),
            "denominator": "planned_cells",
            "complete": complete,
            "arms": {arm: _headline_arm(arms[arm]) for arm in sorted(arms)},
        },
        "completeness": {
            "passed": complete,
            "planned_cells": len(planned),
            "observed_cells": len(planned_ids & set(observed)),
            "missing_cells": len(missing),
            "missing": missing,
            "unplanned": unplanned,
        },
        "arms": arms,
        "pairing": {
            "paired_count": len(paired_scores) // 2,
            "unpaired_count": len(unpaired),
            "unpaired": unpaired,
        },
        "deltas": _deltas(paired_arms),
    }
