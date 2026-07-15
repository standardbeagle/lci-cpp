"""Cited-evidence scoring for the Stage-1 exploration benchmark."""

from scoring.citations import normalize_path, parse_citations
from scoring.scorer import (
    AGGREGATE_SCHEMA,
    SCORE_SCHEMA,
    IncompatibleRuns,
    aggregate,
    score_run,
)

__all__ = [
    "AGGREGATE_SCHEMA",
    "SCORE_SCHEMA",
    "IncompatibleRuns",
    "aggregate",
    "normalize_path",
    "parse_citations",
    "score_run",
]
