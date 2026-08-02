"""Deterministic scoring for claim-validation runs.

No model judges this benchmark.  Verdicts and citations are parsed from the
runner's constrained JSON answer and checked against the adjudicated, forged
corpus coordinates in the task bank.
"""

import json
import posixpath
import statistics

import sealed_artifacts
from scoring.citations import normalize_path
from scoring.pairing import IncompatibleRuns, collect_latest, pair_cells
from scoring.scorer import distribution

CLAIM_SCORE_SCHEMA = "claim_validation_score_v1"
CLAIM_AGGREGATE_SCHEMA = "claim_validation_aggregate_v1"
CLAIM_SCORE_SET_SCHEMA = "claim_validation_score_set_v1"

_VERDICTS = frozenset({"true", "false", "unsupported"})
_COMPAT_FIELDS = (
    "task_bank_digest", "forge_manifest", "model", "mode",
    "schema_version", "run_settings", "scoring_settings",
)


def _unsafe_citation_path(path):
    """A citation path must be a plain relative location inside the checkout.

    Traversal and absolute forms are checked on the backslash-normalised path so
    a Windows-style separator cannot smuggle `..` past a POSIX-only split, and
    sealed harness locations are refused here too -- an answer the runner gates
    away must not be scoreable after the fact."""
    normal = path.replace("\\", "/")
    return (path.startswith(("/", "\\")) or posixpath.isabs(normal)
            or ".." in normal.split("/")
            or sealed_artifacts.is_sealed_path(path))


def parse_claim_answer_result(value):
    """Return ``(answer, canonical, reason)`` for one claim answer.

    Single source of truth for the claim-answer schema: the runner gates a run
    on `reason` while the scorer scores `canonical`, so both see one verdict on
    every payload. `answer` is the raw decoded object the runner persists;
    `canonical` carries deduped, normalised citations. On rejection both are
    ``None`` and `reason` names the failing check."""
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except (TypeError, json.JSONDecodeError):
            return None, None, "invalid_json"
    if not isinstance(value, dict) or set(value) != {"verdict", "evidence", "rationale"}:
        return None, None, "fields"
    verdict = value["verdict"]
    evidence = value["evidence"]
    rationale = value["rationale"]
    if verdict not in _VERDICTS:
        return None, None, "verdict"
    if not isinstance(rationale, str) or not rationale.strip():
        return None, None, "rationale"
    if not isinstance(evidence, list) or not evidence:
        return None, None, "evidence"
    citations = set()
    for citation in evidence:
        if not isinstance(citation, dict) or set(citation) != {"path", "line"}:
            return None, None, "citation"
        path, line = citation["path"], citation["line"]
        if (not isinstance(path, str) or not path or _unsafe_citation_path(path)
                or not isinstance(line, int) or isinstance(line, bool) or line < 1):
            return None, None, "citation"
        citations.add((normalize_path(path), line, line))
    canonical = {"verdict": verdict, "evidence": sorted(citations),
                 "rationale": rationale.strip()}
    return value, canonical, None


def parse_claim_answer(value):
    """Return a canonical answer or ``None`` for any malformed ambiguity."""
    return parse_claim_answer_result(value)[1]


def _within(cited, anchor):
    path, start, end = cited
    key_path = normalize_path(anchor["path"])
    lines = anchor["lines"]
    return path == key_path and start >= lines[0] and end <= lines[-1]


def _evidence(task, citations):
    anchors = task["evidence"]
    classes = task["author"]["anchor_classification"]
    valid = 0
    matched = set()
    acceptable = set()
    dead_or_misleading = set()
    for index, classification in enumerate(classes):
        (acceptable if classification == "authoritative-live" else dead_or_misleading).add(index)
    for citation in citations:
        hits = {i for i, anchor in enumerate(anchors) if _within(citation, anchor)}
        live_hits = hits & acceptable
        # Ambiguous coordinates fail closed: a live anchor cannot launder a
        # citation that also points at any forbidden/misleading anchor.
        if live_hits and not (hits & dead_or_misleading):
            valid += 1
            matched.update(live_hits)
    total = len(citations)
    precision = valid / total if total else 0.0
    recall = len(matched) / len(acceptable) if acceptable else 0.0
    f1 = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
    return {
        "precision": precision, "recall": recall, "f1": f1,
        "cited_total": total, "cited_valid": valid,
        "cited_invalid": total - valid, "answer_key_total": len(acceptable),
        "answer_key_matched": len(matched),
    }


def _null_evidence(task):
    total = sum(c == "authoritative-live" for c in task["author"]["anchor_classification"])
    return {"precision": None, "recall": None, "f1": None, "cited_total": 0,
            "cited_valid": 0, "cited_invalid": 0, "answer_key_total": total,
            "answer_key_matched": 0}


def score_claim_run(task, record, *, task_bank_digest=None, settings=None):
    """Score one run. Infrastructure/malformed failures remain explicit."""
    run_settings = record.get("settings") or {}
    effective_settings = settings if settings is not None else run_settings
    min_valid = effective_settings.get("evidence_min_valid", 1)
    min_recall = effective_settings.get("evidence_min_recall", 0.0)
    if (not isinstance(min_valid, int) or isinstance(min_valid, bool) or min_valid < 1
            or not isinstance(min_recall, (int, float)) or isinstance(min_recall, bool)
            or not 0 <= min_recall <= 1):
        raise ValueError("invalid configured evidence rule")
    answered = record.get("status") == "answered"
    parsed = parse_claim_answer(record.get("structured_answer")) if answered else None
    valid_answer = parsed is not None
    evidence = _evidence(task, parsed["evidence"]) if valid_answer else _null_evidence(task)
    expected = task["author"]["verdict"]
    predicted = parsed["verdict"] if parsed else None
    verdict_correct = predicted == expected if parsed else False
    grounded = bool(valid_answer and evidence["cited_valid"] >= min_valid
                    and evidence["recall"] >= min_recall)
    success = bool(verdict_correct and grounded)
    tokens = record.get("token_usage") or {}
    return {
        "schema": CLAIM_SCORE_SCHEMA, "run_key": record.get("run_key"),
        "task_id": task["id"], "seed": record.get("seed"), "arm": record.get("arm"),
        "category": task["author"]["category"], "expected_verdict": expected,
        "predicted_verdict": predicted, "status": record.get("status"),
        "valid_answer": valid_answer, "verdict_correct": verdict_correct,
        "grounded": grounded, "success": success, "evidence": evidence,
        "model": record.get("model"), "mode": record.get("mode"),
        "schema_version": record.get("schema_version"),
        # forge_version is the cross-corpus manifest compatibility knob; the
        # tree hash itself legitimately differs for every forged corpus.
        "forge_manifest": record.get("forge_version"),
        "claim_task_digest": record.get("claim_task_digest"),
        "task_bank_digest": task_bank_digest or record.get("task_bank_digest"),
        # Run configuration and post-hoc scoring policy are independent
        # provenance dimensions.  In particular, an explicit evidence-policy
        # override must never erase experimental settings and make unlike arms
        # appear pairable.
        "run_settings": run_settings,
        "scoring_settings": effective_settings,
        "process": {"tool_calls": len(record.get("tool_calls") or []),
                    "input_tokens": tokens.get("input"), "output_tokens": tokens.get("output"),
                    "wall_clock_seconds": record.get("duration_seconds") or 0.0},
    }


def _rate(items, field):
    return sum(bool(item[field]) for item in items) / len(items) if items else 0.0


def _arm_summary(scores):
    answered = [s for s in scores if s["valid_answer"]]
    categories = {}
    for category in sorted({s["category"] for s in scores}):
        cells = [s for s in scores if s["category"] == category]
        confusion = {truth: {pred: 0 for pred in sorted(_VERDICTS | {"invalid"})}
                     for truth in sorted(_VERDICTS)}
        for cell in cells:
            confusion[cell["expected_verdict"]][cell["predicted_verdict"] or "invalid"] += 1
        categories[category] = {"count": len(cells), "confusion": confusion}
    ev = answered
    macro = lambda key: statistics.mean(s["evidence"][key] for s in ev) if ev else None
    return {
        "count": len(scores), "answered": len(answered),
        "verdict_accuracy": _rate(scores, "verdict_correct"),
        "grounded_accuracy": _rate(scores, "success"),
        "citation": {"precision": macro("precision"), "recall": macro("recall"), "f1": macro("f1")},
        "false_premise_rate": _rate([s for s in scores if s["expected_verdict"] == "false"], "success"),
        "unsupported_rate": _rate([s for s in scores if s["expected_verdict"] == "unsupported"], "success"),
        "categories": categories,
        "tool_calls": distribution([s["process"]["tool_calls"] for s in scores]),
    }


def _attempt_view(score):
    attempt = {field: score.get(field) for field in
               ("status", "valid_answer", "predicted_verdict", "success", "process")}
    return score.get("attempts") or [attempt]


def _latest_and_pair(scores):
    # Latest-wins with the completion guard and completed-cells-pair convention
    # both live in scoring.pairing -- one implementation for every scorer.
    latest, attempts = collect_latest(scores, _attempt_view)
    paired, unpaired = pair_cells(latest)
    return latest, paired, unpaired, attempts


def aggregate_claim_scores(score_records):
    scores, paired, unpaired, attempts = _latest_and_pair(list(score_records))
    if not scores:
        raise IncompatibleRuns("no score records to aggregate")
    compatibility = {}
    for field in _COMPAT_FIELDS:
        values = {json.dumps(s.get(field), sort_keys=True) for s in scores}
        if len(values) != 1 or next(iter(values)) == "null":
            raise IncompatibleRuns(f"records have missing or incompatible {field!r}")
        compatibility[field] = scores[0].get(field)
    arms = {arm: _arm_summary([s for s in scores if s["arm"] == arm])
            for arm in sorted({s["arm"] for s in scores})}
    paired_arms = {arm: _arm_summary([s for s in paired if s["arm"] == arm])
                   for arm in sorted({s["arm"] for s in paired})}
    deltas = {}
    if set(paired_arms) >= {"treatment", "baseline"}:
        for field in ("verdict_accuracy", "grounded_accuracy", "false_premise_rate", "unsupported_rate"):
            deltas[field] = paired_arms["treatment"][field] - paired_arms["baseline"][field]
        for field in ("precision", "recall", "f1"):
            a, b = paired_arms["treatment"]["citation"][field], paired_arms["baseline"]["citation"][field]
            deltas["citation_" + field] = None if a is None or b is None else a - b
    return {"schema": CLAIM_AGGREGATE_SCHEMA, "compatibility": compatibility,
            "arms": arms, "pairing": {"paired_count": len(paired) // 2,
            "unpaired_count": len(unpaired), "unpaired": unpaired}, "deltas": deltas,
            "attempts": [{"run_key": key, "attempts": value}
                         for key, value in sorted(attempts.items())]}
