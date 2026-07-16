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
    _distribution,
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
        "LCI_ABSENT",
        "BLAST_RADIUS_MALFORMED",
        "RULE_MALFORMED",
        "RULE_UNSUPPORTED",
        REASON_GATE_ABSENT,
    }
)

# The gate ran, but the oracle/anchor material it judged with is itself
# defective -- the cell yields no evidence either way about the patch.
_ORACLE_REASONS = frozenset(
    {
        "NON_DISCRIMINATING",
        "WRONG_DIRECTION",
        "ANCHOR_DECOY",
        "ANCHOR_NOT_LIVE",
        "ANCHOR_MISSING",
        "ANCHOR_BOUND_STALE",
        "ANCHOR_IDENTIFIER_ABSENT",
        "MATCH_AMBIGUOUS",
    }
)

_PASSING_REASONS = frozenset(
    {"SUITE_GREEN", "DISCRIMINATES", "WITHIN_SCOPE", "NO_ESCAPE", "CONFORMS"}
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
    derived[GATE_CONVENTION] = (
        _absent_gate() if conformance is None else _derive_gate([conformance])
    )
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
    if reasons & _HARNESS_REASONS:
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
