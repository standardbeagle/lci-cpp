"""Deterministic scoring for claim-validation runs.

No model judges this benchmark.  Verdicts and citations are parsed from the
runner's constrained JSON answer and checked against the adjudicated, forged
corpus coordinates in the task bank.
"""

import json
import statistics

from scoring.citations import normalize_path
from scoring.scorer import IncompatibleRuns, _distribution

CLAIM_SCORE_SCHEMA = "claim_validation_score_v1"
CLAIM_AGGREGATE_SCHEMA = "claim_validation_aggregate_v1"
CLAIM_SCORE_SET_SCHEMA = "claim_validation_score_set_v1"

_VERDICTS = frozenset({"true", "false", "unsupported"})
_COMPAT_FIELDS = (
    "task_bank_digest", "forge_manifest", "model", "mode",
    "schema_version", "settings",
)


def parse_claim_answer(value):
    """Return a canonical answer or ``None`` for any malformed ambiguity."""
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except (TypeError, json.JSONDecodeError):
            return None
    if not isinstance(value, dict) or set(value) != {"verdict", "evidence", "rationale"}:
        return None
    verdict = value.get("verdict")
    evidence = value.get("evidence")
    rationale = value.get("rationale")
    if verdict not in _VERDICTS or not isinstance(rationale, str) or not rationale.strip():
        return None
    if not isinstance(evidence, list) or not evidence:
        return None
    citations = set()
    for citation in evidence:
        if not isinstance(citation, dict) or set(citation) != {"path", "line"}:
            return None
        path, line = citation["path"], citation["line"]
        if (not isinstance(path, str) or not path or path.startswith(("/", "\\"))
                or ".." in path.replace("\\", "/").split("/")
                or not isinstance(line, int) or isinstance(line, bool) or line < 1):
            return None
        citations.add((normalize_path(path), line, line))
    return {"verdict": verdict, "evidence": sorted(citations), "rationale": rationale.strip()}


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
        if live_hits:
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
    effective_settings = settings if settings is not None else (record.get("settings") or {})
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
        "task_bank_digest": task_bank_digest or record.get("task_bank_digest"),
        "settings": effective_settings,
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
        "tool_calls": _distribution([s["process"]["tool_calls"] for s in scores]),
    }


def _latest_and_pair(scores):
    latest = {}
    for score in scores:
        key = score.get("run_key")
        if key is None:
            raise IncompatibleRuns("score record is missing required run_key")
        latest[key] = score
    cells = {}
    for score in latest.values():
        key = (score.get("task_id"), score.get("seed"))
        arm = score.get("arm")
        if arm in cells.setdefault(key, {}):
            raise IncompatibleRuns("multiple run keys claim the same task/seed/arm cell")
        cells[key][arm] = score
    paired, unpaired = [], []
    for key, arms in sorted(cells.items()):
        if set(arms) >= {"treatment", "baseline"}:
            paired.extend((arms["treatment"], arms["baseline"]))
        else:
            unpaired.extend({"task_id": key[0], "seed": key[1], "arm": arm,
                             "run_key": score["run_key"], "status": score["status"]}
                            for arm, score in arms.items())
    return list(latest.values()), paired, unpaired


def aggregate_claim_scores(score_records):
    scores, paired, unpaired = _latest_and_pair(list(score_records))
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
            "unpaired_count": len(unpaired), "unpaired": unpaired}, "deltas": deltas}
