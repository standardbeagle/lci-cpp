"""Cited-evidence scoring for the Stage-1 exploration benchmark."""

from scoring.citations import normalize_path, parse_citations
from scoring.scorer import (
    AGGREGATE_SCHEMA,
    SCORE_SCHEMA,
    IncompatibleRuns,
    aggregate,
    score_run,
)
from scoring.claim_validation import (
    CLAIM_AGGREGATE_SCHEMA,
    CLAIM_SCORE_SCHEMA,
    CLAIM_SCORE_SET_SCHEMA,
    aggregate_claim_scores,
    parse_claim_answer,
    score_claim_run,
)

__all__ = [
    "AGGREGATE_SCHEMA",
    "SCORE_SCHEMA",
    "IncompatibleRuns",
    "aggregate",
    "normalize_path",
    "parse_citations",
    "score_run",
    "CLAIM_AGGREGATE_SCHEMA",
    "CLAIM_SCORE_SCHEMA",
    "CLAIM_SCORE_SET_SCHEMA",
    "aggregate_claim_scores",
    "parse_claim_answer",
    "score_claim_run",
]
