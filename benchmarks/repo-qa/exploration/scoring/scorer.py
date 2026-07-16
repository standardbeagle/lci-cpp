"""Score exploration runs by CITED EVIDENCE, then aggregate the two arms.

Primary outcome is cited-evidence precision/recall/F1 against the adjudicated
answer key -- deliberately NOT the repo-QA regex fact score and NOT an LLM judge.
Process metrics (tool calls, tokens, wall-clock) ride alongside as secondary
signal. A cited anchor is VALID only when its line span lies fully within a
declared answer-key anchor's bounds on the same file; anything else (unknown
path, out-of-bounds line, whole-file spray) is a false positive. Failed S4 runs
are preserved with their status and carry NULL evidence, never a scored empty
answer.

`aggregate` pairs the arms and emits treatment-minus-baseline deltas. It refuses
to fold records that disagree on model, forge version, or task-bank version into
one paired aggregate unless the caller explicitly opts in via `group_by`.
"""

import statistics

from scoring.citations import normalize_path, parse_citations

SCORE_SCHEMA = "exploration_score_v1"
AGGREGATE_SCHEMA = "exploration_aggregate_v1"

_ANSWERED = "answered"

# Fields that must be homogeneous within one paired aggregate. Corpus id and
# source commit vary legitimately across a multi-corpus bank, so they are NOT
# here; forge_version is the forge-manifest compatibility knob.
_COMPAT_FIELDS = ("model", "forge_version", "task_bank_version")


class IncompatibleRuns(Exception):
    """Records that disagree on a compatibility field were aggregated together
    without the caller explicitly grouping by it -- fail loud."""


# ---------------------------------------------------------------------------
# per-run scoring
# ---------------------------------------------------------------------------


def _within(cited, key_anchor):
    """True if a cited `(path, cs, ce)` lies fully inside a key anchor."""
    cpath, cstart, cend = cited
    kpath = normalize_path(key_anchor["path"])
    if not (cpath == kpath or cpath.endswith("/" + kpath)):
        return False
    lines = key_anchor["lines"]
    kstart, kend = lines[0], lines[-1]
    return cstart >= kstart and cend <= kend


def _score_evidence(task, final_answer):
    """Precision / recall / F1 of the answer's citations vs the answer key.

    Precision = valid distinct cited anchors / all distinct cited anchors
                (0.0 when nothing was cited).
    Recall    = answer-key anchors matched / total answer-key anchors.
    F1        = harmonic mean (0.0 when precision + recall is 0).
    """
    key = task["evidence"]
    cited = parse_citations(final_answer)

    valid = 0
    matched_key = set()
    for anchor in cited:
        hit = False
        for index, key_anchor in enumerate(key):
            if _within(anchor, key_anchor):
                matched_key.add(index)
                hit = True
        if hit:
            valid += 1

    cited_total = len(cited)
    key_total = len(key)
    precision = valid / cited_total if cited_total else 0.0
    recall = len(matched_key) / key_total if key_total else 0.0
    f1 = (
        2 * precision * recall / (precision + recall)
        if (precision + recall)
        else 0.0
    )
    return {
        "precision": precision,
        "recall": recall,
        "f1": f1,
        "answer_key_total": key_total,
        "answer_key_matched": len(matched_key),
        "cited_total": cited_total,
        "cited_valid": valid,
        "cited_invalid": cited_total - valid,
    }


def _null_evidence(task):
    """Evidence block for a run with no answer to score (a failure)."""
    return {
        "precision": None,
        "recall": None,
        "f1": None,
        "answer_key_total": len(task["evidence"]),
        "answer_key_matched": 0,
        "cited_total": 0,
        "cited_valid": 0,
        "cited_invalid": 0,
    }


def _process(record):
    tokens = record.get("token_usage") or {}
    return {
        "tool_calls": len(record.get("tool_calls") or []),
        "input_tokens": tokens.get("input"),
        "output_tokens": tokens.get("output"),
        "wall_clock_seconds": record.get("duration_seconds") or 0.0,
    }


def score_run(task, record):
    """Score one S4 run record against its task answer key.

    Returns a schema-versioned score dict. A non-`answered` status yields NULL
    evidence so failures never masquerade as zero-scored empty answers.
    """
    status = record.get("status")
    answered = status == _ANSWERED
    evidence = (
        _score_evidence(task, record.get("final_answer"))
        if answered
        else _null_evidence(task)
    )
    return {
        "schema": SCORE_SCHEMA,
        "run_key": record.get("run_key"),
        "task_id": record.get("task_id"),
        "seed": record.get("seed"),
        "corpus_id": record.get("corpus_id"),
        "arm": record.get("arm"),
        "model": record.get("model"),
        "forge_version": record.get("forge_version"),
        "task_bank_version": task.get("schema"),
        "status": status,
        "answered": answered,
        "evidence": evidence,
        "process": _process(record),
    }


# ---------------------------------------------------------------------------
# aggregation
# ---------------------------------------------------------------------------


def _distribution(values):
    """min / median / mean / max / total over a numeric sample, or None."""
    nums = [v for v in values if v is not None]
    if not nums:
        return None
    return {
        "n": len(nums),
        "min": min(nums),
        "median": statistics.median(nums),
        "mean": statistics.mean(nums),
        "max": max(nums),
        "total": sum(nums),
    }


def _macro_evidence(scores):
    """Macro-average P/R/F1 over ANSWERED runs only (failures are excluded so a
    timeout cannot be laundered into a zero-scored answer)."""
    answered = [s for s in scores if s["answered"]]
    if not answered:
        return {"precision": None, "recall": None, "f1": None, "answered_n": 0}
    return {
        "precision": statistics.mean(s["evidence"]["precision"] for s in answered),
        "recall": statistics.mean(s["evidence"]["recall"] for s in answered),
        "f1": statistics.mean(s["evidence"]["f1"] for s in answered),
        "answered_n": len(answered),
    }


def _arm_summary(scores):
    count = len(scores)
    answered = sum(1 for s in scores if s["answered"])
    return {
        "count": count,
        "answered": answered,
        "success_rate": answered / count if count else 0.0,
        "evidence": _macro_evidence(scores),
        "tool_calls": _distribution([s["process"]["tool_calls"] for s in scores]),
        "input_tokens": _distribution([s["process"]["input_tokens"] for s in scores]),
        "output_tokens": _distribution(
            [s["process"]["output_tokens"] for s in scores]
        ),
        "wall_clock_seconds": _distribution(
            [s["process"]["wall_clock_seconds"] for s in scores]
        ),
    }


def _check_compatibility(scores, group_by):
    """Fail loud on a heterogeneous compatibility field the caller did not opt
    into mixing. Returns the shared (or explicitly grouped) field values."""
    grouped = set(group_by or ())
    shared = {}
    for field in _COMPAT_FIELDS:
        values = sorted({s.get(field) for s in scores})
        if len(values) > 1 and field not in grouped:
            raise IncompatibleRuns(
                f"records disagree on {field!r} ({values}); pass "
                f"group_by=[{field!r}] to aggregate them intentionally"
            )
        shared[field] = values[0] if len(values) == 1 else values
    return shared


def _delta(treatment, baseline):
    if treatment is None or baseline is None:
        return None
    return treatment - baseline


def _deltas(arms):
    """LCI-minus-baseline deltas; None whenever an arm or metric is absent."""
    t = arms.get("treatment")
    b = arms.get("baseline")
    if t is None or b is None:
        return {}
    te, be = t["evidence"], b["evidence"]
    tw, bw = t["wall_clock_seconds"], b["wall_clock_seconds"]
    tc, bc = t["tool_calls"], b["tool_calls"]
    ti, bi = t["input_tokens"], b["input_tokens"]
    to, bo = t["output_tokens"], b["output_tokens"]
    return {
        "success_rate": _delta(t["success_rate"], b["success_rate"]),
        "precision": _delta(te["precision"], be["precision"]),
        "recall": _delta(te["recall"], be["recall"]),
        "f1": _delta(te["f1"], be["f1"]),
        "tool_calls_mean": _delta(
            tc["mean"] if tc else None, bc["mean"] if bc else None
        ),
        "input_tokens_mean": _delta(
            ti["mean"] if ti else None, bi["mean"] if bi else None
        ),
        "output_tokens_mean": _delta(
            to["mean"] if to else None, bo["mean"] if bo else None
        ),
        "wall_clock_mean": _delta(
            tw["mean"] if tw else None, bw["mean"] if bw else None
        ),
    }


def _latest_by_run_key(scores):
    """Keep the last score for each run key, matching append-log semantics."""
    latest = {}
    for score in scores:
        key = score.get("run_key")
        if key is None:
            raise IncompatibleRuns("score record is missing required run_key")
        latest[key] = score
    return list(latest.values())


def _pair_scores(scores):
    """Return exact task/seed pairs and a forensic list of unmatched cells."""
    cells = {}
    for score in scores:
        cell = (score.get("task_id"), score.get("seed"))
        arm = score.get("arm")
        arms = cells.setdefault(cell, {})
        if arm in arms:
            previous = arms[arm]
            raise IncompatibleRuns(
                "multiple run keys claim the same task/seed/arm cell: "
                f"{previous.get('run_key')!r} and {score.get('run_key')!r}"
            )
        arms[arm] = score

    paired = []
    unpaired = []
    for (task_id, seed), arms in sorted(
        cells.items(), key=lambda item: (str(item[0][0]), str(item[0][1]))
    ):
        treatment = arms.get("treatment")
        baseline = arms.get("baseline")
        if treatment is not None and baseline is not None:
            paired.extend((treatment, baseline))
        for arm, score in sorted(arms.items(), key=lambda item: str(item[0])):
            if treatment is None or baseline is None:
                unpaired.append({
                    "task_id": task_id,
                    "seed": seed,
                    "arm": arm,
                    "run_key": score.get("run_key"),
                    "status": score.get("status"),
                })
    return paired, unpaired


def aggregate(score_records, *, group_by=()):
    """Pair the arms of a homogeneous run set and emit deltas.

    Refuses (raises IncompatibleRuns) if the records disagree on model, forge
    version, or task-bank version unless that field is named in `group_by`.
    """
    scores = _latest_by_run_key(list(score_records))
    if not scores:
        raise IncompatibleRuns("no score records to aggregate")

    shared = _check_compatibility(scores, group_by)

    by_arm = {}
    for score in scores:
        by_arm.setdefault(score["arm"], []).append(score)
    arms = {arm: _arm_summary(sorted(by_arm[arm], key=lambda s: s["run_key"]))
            for arm in sorted(by_arm)}

    paired_scores, unpaired = _pair_scores(scores)
    paired_by_arm = {}
    for score in paired_scores:
        paired_by_arm.setdefault(score["arm"], []).append(score)
    paired_arms = {
        arm: _arm_summary(paired_by_arm[arm]) for arm in sorted(paired_by_arm)
    }

    return {
        "schema": AGGREGATE_SCHEMA,
        "grouped_by": sorted(group_by or ()),
        "compatibility": shared,
        "arms": arms,
        "pairing": {
            "paired_count": len(paired_scores) // 2,
            "unpaired_count": len(unpaired),
            "unpaired": unpaired,
        },
        "deltas": _deltas(paired_arms),
    }
