"""Drive family x cohort x arm x level, resume-safe, and report both levels.

Every cell is run at TWO measurement levels because they answer different
questions:

  * `tool`  -- the direct MCP / direct baseline-tool answer. Is LCI CORRECT
               against the gopls key? Cheap, deterministic, no model variance.
  * `agent` -- the same cell driven through an agent. Does an agent WITH LCI
               beat the grep-only agent? That is the product claim.

They are reported separately and never collapsed into one number. A family that
is tool-level correct with no agent-level lift is named in
`tool_correct_no_agent_lift`: that gap is a prompting/adoption problem, a
different fix from a capability gap, and averaging the two levels together
hides which one is being observed.

Resume-safety is the existing bench.py pattern: one JSON file per
(family, arm, level, cell), and a file that exists is not re-run.

Nothing here starts an agent or an MCP server -- `executor` is injected.
"""

import json
import os
import tempfile

from . import answer_sets, grading

REPORT_SCHEMA = "discovery_sweep_report_v1"
ROW_SCHEMA = "discovery_sweep_row_v1"
PLAN_FILE = "_plan.json"

STATUS_OK = "ok"
STATUS_DNF = "dnf"

# A treatment this accurate at the tool level has the capability; anything
# missing at the agent level is then adoption, not capability.
TOOL_CORRECT_F1 = 0.8

_METRIC_KEYS = {
    "mean_f1": "f1",
    "mean_precision": "precision",
    "mean_recall": "recall",
}


class BrokenCellError(RuntimeError):
    """A cell could not be run or graded.

    Raised rather than written, because a zeroed row for a cell whose oracle or
    arm broke reads downstream as a legitimate loss for that arm.
    """


def _write_json_atomic(path, payload):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True)
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise


def row_name(family_id, arm, level, slug):
    return "%s__%s__%s__%s.json" % (family_id, arm, level, slug)


def _truth_for(family, cell, answer_key_for, literal_corpus_root):
    if family["task_shape"] == "literal_string_search":
        if literal_corpus_root is None:
            raise BrokenCellError(
                "family %s needs literal_corpus_root to build its own oracle"
                % family["id"]
            )
        return answer_sets.literal_answer_set(literal_corpus_root, cell["name"])
    try:
        key = answer_key_for(cell)
    except Exception as exc:  # the oracle refuses loudly; do not grade past it
        raise BrokenCellError(
            "answer key unavailable for %s/%s: %s" % (family["id"], cell["slug"], exc)
        )
    return answer_sets.answer_set_for(family["task_shape"], key)


def run_sweep(plan, answer_key_for, executor, run_dir, literal_corpus_root=None):
    """Execute (or resume) every cell of the plan. Returns run counters."""
    os.makedirs(run_dir, exist_ok=True)
    _write_json_atomic(os.path.join(run_dir, PLAN_FILE), plan)

    executed = 0
    resumed = 0
    dnf = 0
    for family in plan["families"]:
        for cell in family["cells"]:
            truth = None
            for level in plan["levels"]:
                for arm in plan["arms"]:
                    path = os.path.join(
                        run_dir, row_name(family["id"], arm, level, cell["slug"])
                    )
                    if os.path.exists(path):
                        resumed += 1
                        continue
                    if truth is None:
                        truth = _truth_for(
                            family, cell, answer_key_for, literal_corpus_root
                        )
                    job = {
                        "family": family["id"],
                        "task_shape": family["task_shape"],
                        "arm": arm,
                        "level": level,
                        "slug": cell["slug"],
                        "cell": cell,
                        "prompt": cell["prompt"],
                        "answer_set_total": len(truth["locations"]),
                    }
                    result = executor(job)
                    status = result.get("status")
                    if status == STATUS_DNF:
                        score = grading.null_score(STATUS_DNF)
                        dnf += 1
                    elif status == STATUS_OK:
                        score = grading.score_answer(result.get("answer", ""), truth)
                    else:
                        raise BrokenCellError(
                            "cell %s/%s/%s/%s is broken (status=%r): %s"
                            % (family["id"], arm, level, cell["slug"], status,
                               result.get("detail", "no detail reported"))
                        )
                    _write_json_atomic(path, {
                        "schema_version": ROW_SCHEMA,
                        "family": family["id"],
                        "role": family["role"],
                        "task_shape": family["task_shape"],
                        "threshold": family.get("threshold"),
                        "arm": arm,
                        "level": level,
                        "slug": cell["slug"],
                        "status": status,
                        "dnf_reason": result.get("reason"),
                        "answer": result.get("answer", ""),
                        "score": score,
                        "gradable": bool(truth["gradable"]),
                        "answer_set_total": len(truth["locations"]),
                        "tool_calls": result.get("tool_calls"),
                        "tokens": result.get("tokens"),
                        "wall_seconds": result.get("wall_seconds"),
                    })
                    executed += 1
    return {"executed": executed, "resumed": resumed, "dnf": dnf}


def _is_row_file(name):
    """Rows are named family__arm__level__slug.json.

    The run directory also holds the plan and the emitted report, and reading
    one of those as a row is how a second `run` of an already-complete sweep
    crashed instead of reprinting its report.
    """
    return name.endswith(".json") and name.count("__") == 3


def _load_rows(run_dir):
    rows = []
    for name in sorted(os.listdir(run_dir)):
        if not _is_row_file(name):
            continue
        with open(os.path.join(run_dir, name), encoding="utf-8") as handle:
            rows.append(json.load(handle))
    return rows


def _summarize(rows):
    total = len(rows)
    dnf_rows = [r for r in rows if r["status"] == STATUS_DNF]
    graded = [r for r in rows
              if r["status"] == STATUS_OK and r["score"]["f1"] is not None]
    ungradable = [r for r in rows
                  if r["status"] == STATUS_OK and r["score"]["f1"] is None]
    summary = {
        "cell_count": total,
        "graded_count": len(graded),
        "dnf_count": len(dnf_rows),
        # DNF is a first-class outcome with its own rate; it is NOT folded into
        # the accuracy means as a zero, which would blur "gave a wrong answer"
        # into "never answered".
        "dnf_rate_pct": round(100.0 * len(dnf_rows) / total, 2) if total else None,
        "ungradable_count": len(ungradable),
        "ungradable_reasons": sorted({r["score"]["ungradable_reason"]
                                      for r in ungradable
                                      if r["score"]["ungradable_reason"]}),
        "correct_count": sum(1 for r in graded if r["score"]["correct"]),
        # An arm that cited NOTHING scores 0.0 the same way an arm that cited
        # only wrong locations does, and the two mean different things: the
        # first can be an arm whose tool emits no path:line at all. Counted
        # here so a 0.0 family cannot be read as "wrong" without checking.
        "zero_citation_count": sum(1 for r in graded if r["score"]["cited_total"] == 0),
    }
    for name, key in _METRIC_KEYS.items():
        summary[name] = (
            round(sum(r["score"][key] for r in graded) / len(graded), 6)
            if graded else None
        )
    return summary


def _lift(level_summary, metric):
    treatment = level_summary.get("treatment", {}).get(metric)
    baseline = level_summary.get("baseline", {}).get(metric)
    if treatment is None or baseline is None:
        return None
    return round(treatment - baseline, 6)


def build_report(run_dir):
    """Aggregate the written rows. Reads only what the sweep persisted."""
    rows = _load_rows(run_dir)
    plan_path = os.path.join(run_dir, PLAN_FILE)
    plan = {}
    if os.path.exists(plan_path):
        with open(plan_path, encoding="utf-8") as handle:
            plan = json.load(handle)

    levels = {}
    thresholds = {}
    for row in rows:
        thresholds.setdefault(row["family"], row.get("threshold"))
        bucket = levels.setdefault(row["level"], {}).setdefault(row["family"], {})
        bucket.setdefault(row["arm"], []).append(row)
    for level, families in levels.items():
        for family, arms in families.items():
            families[family] = {arm: _summarize(rs) for arm, rs in arms.items()}

    adoption_gap = []
    unmeasurable = []
    for family, threshold in thresholds.items():
        threshold = threshold or {}
        metric = threshold.get("metric", "mean_f1")
        if metric not in _METRIC_KEYS or threshold.get("operator") != ">=":
            continue
        tool = levels.get("tool", {}).get(family, {})
        agent = levels.get("agent", {}).get(family, {})
        tool_f1 = tool.get("treatment", {}).get("mean_f1")
        tool_lift = _lift(tool, metric)
        agent_lift = _lift(agent, metric)
        if tool_f1 is None or tool_lift is None:
            continue
        if tool_f1 < TOOL_CORRECT_F1 or tool_lift < threshold["value"]:
            continue
        if agent_lift is None:
            unmeasurable.append(family)
        elif agent_lift < threshold["value"]:
            adoption_gap.append(family)

    return {
        "schema_version": REPORT_SCHEMA,
        "registry_id": plan.get("registry_id"),
        "corpus_commit": plan.get("corpus_commit"),
        "cohort_reduction": plan.get("cohort_reduction"),
        "voided_families": plan.get("voided", []),
        "levels": levels,
        # Named explicitly rather than collapsed: LCI answers the cell correctly
        # through its own surface, but the agent arm gains nothing from it.
        "tool_correct_no_agent_lift": sorted(adoption_gap),
        "tool_correct_agent_level_unmeasurable": sorted(unmeasurable),
    }
