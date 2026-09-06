#!/usr/bin/env python3
"""Analyze the E2.4 tool-description selection grid.

Input: one JSONL ledger per rep (written by scripts/selection_ab.py run), the
committed task bank, and toolcalling/predictions.json. Output: a small
machine-readable JSON under toolcalling/analysis/ plus the numbers the Markdown
report quotes.

What this measures, and what it does not: the grid asks which tool a model
CALLS FIRST for a stated goal, against a mock MCP that returns deterministic
stubs. It does not show that the real tool then answers correctly (epic 1), and
it does not compare an end-to-end agent against grep (discovery epic).

Three rules do the work, and each is a pure function the tests re-inject
known-bad input into:

  ordering    a prediction registry that does not demonstrably predate the
              records is not a prediction. Every ledger must carry time
              evidence -- per-record `timestamp` or a `<ledger>.meta.json`
              sidecar with `started_at` -- and it must be later than the
              predictions commit. No evidence is a hard error, never a pass.
  spread      a variant is credited only when its gap over the baseline exceeds
              the baseline's own run-to-run spread. Selection on a weak model
              is noisy; a single grid cannot tell a wording effect from a
              resample.
  neighbor    a recall gain on a tool's own task that arrives with that tool
              being wrongly reached for MORE often on the tasks that name it as
              a confusable neighbor is a REGRESSION. Reporting it as a win is
              how a description change ships net-negative.

Non-selection outcomes (provider_*, exit_*, no_tool_call, empty_answer,
hallucinated_tool) are excluded from every denominator and reported by name; a
provider outage is not evidence that a model picked the wrong tool.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
BENCH_ROOT = HERE.parent
TASKS_DIR = BENCH_ROOT / "toolcalling/tasks"
PREDICTIONS = BENCH_ROOT / "toolcalling/predictions.json"
DEFAULT_OUT = BENCH_ROOT / "toolcalling/analysis"

BOUNDARY = (
    "Measures TOOL SELECTION against a mock MCP serving deterministic stubs. It "
    "does not show that the selected tool then produces a correct answer (epic 1), "
    "and it does not show that an LCI-equipped agent beats grep end to end "
    "(discovery epic)."
)

CAVEATS = [
    "Denominators exclude every non-selection outcome; n is reported beside every rate.",
    "A native:* first call (the agent reaching for its built-in bash/read before the "
    "offered toolset) IS a selection outcome and keeps its own confusion column.",
    "Only the FIRST call is graded. A run that reached the right tool third has "
    "mis-selected.",
]


class LedgerError(RuntimeError):
    """A ledger cannot be trusted: no time evidence, or unreadable."""


class OrderingError(RuntimeError):
    """Records predate the pre-registered predictions."""


# ------------------------------------------------------------------- ledgers

def load_ledger(path) -> dict:
    path = Path(path)
    records = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if line:
            records.append(json.loads(line))
    stamps = [r["timestamp"] for r in records if r.get("timestamp")]
    sidecar = Path(str(path).replace(".jsonl", ".meta.json"))
    started_at = None
    if stamps and len(stamps) == len(records):
        started_at = min(stamps)
    elif sidecar.exists():
        started_at = json.loads(sidecar.read_text()).get("started_at")
    if not started_at:
        raise LedgerError(
            f"{path}: no time evidence (needs a `timestamp` on every record or a "
            f"{sidecar.name} sidecar carrying started_at). Failing closed rather "
            "than assuming the records postdate the predictions.")
    # Last record per run_key wins, mirroring selection_ab.read_records: a cell
    # re-run with --retry-provider-failures supersedes its failed record.
    by_key = {}
    for record in records:
        by_key[record.get("run_key", id(record))] = record
    return {"path": str(path), "started_at": started_at,
            "records": list(by_key.values()), "raw_count": len(records)}


def ordering_problems(predictions_commit_time: str, ledgers) -> list[str]:
    problems = []
    for ledger in ledgers:
        if ledger["started_at"] < predictions_commit_time:
            problems.append(
                f"{ledger['path']} started {ledger['started_at']}, before the "
                f"predictions commit at {predictions_commit_time}")
    return problems


# --------------------------------------------------------------------- rates

def cell_rates(records) -> dict:
    cells: dict[tuple, dict] = {}
    for record in records:
        key = (record["variant"], record["model"], record["tier"])
        cell = cells.setdefault(key, {
            "denominator": 0, "correct": 0,
            "columns": defaultdict(int), "excluded": defaultdict(int)})
        if record.get("counts_in_matrix"):
            cell["denominator"] += 1
            cell["columns"][record["first_called_tool"]] += 1
            if record["first_called_tool"] == record["correct_tool"]:
                cell["correct"] += 1
        else:
            cell["excluded"][record.get("outcome", "unknown")] += 1
    for cell in cells.values():
        cell["columns"] = dict(sorted(cell["columns"].items()))
        cell["excluded"] = dict(sorted(cell["excluded"].items()))
        cell["rate"] = (cell["correct"] / cell["denominator"]
                        if cell["denominator"] else None)
    return cells


def task_rates(records) -> dict:
    """correct-first-call count per (task, variant, model)."""
    out: dict[tuple, dict] = {}
    for record in records:
        key = (record["task_id"], record["variant"], record["model"])
        cell = out.setdefault(key, {"denominator": 0, "correct": 0,
                                    "columns": defaultdict(int)})
        if record.get("counts_in_matrix"):
            cell["denominator"] += 1
            cell["columns"][record["first_called_tool"]] += 1
            if record["first_called_tool"] == record["correct_tool"]:
                cell["correct"] += 1
    for cell in out.values():
        cell["columns"] = dict(sorted(cell["columns"].items()))
        cell["rate"] = (cell["correct"] / cell["denominator"]
                        if cell["denominator"] else None)
    return out


def neighbor_pressure(records, tasks) -> dict:
    """How often each tool is wrongly reached for on tasks that name it a neighbor.

    The denominator is deliberately NOT every task: a tool can only be charged
    with stealing a prompt it was plausibly confusable with.
    """
    neighbors = {t["id"]: set(t.get("confusable_neighbors") or []) for t in tasks}
    out: dict[tuple, dict] = {}
    for record in records:
        if not record.get("counts_in_matrix"):
            continue
        arm = out.setdefault((record["variant"], record["model"]), {})
        for tool in neighbors.get(record["task_id"], ()):  # noqa: B007
            entry = arm.setdefault(tool, {"opportunities": 0, "wrong_calls": 0})
            entry["opportunities"] += 1
            if record["first_called_tool"] == tool:
                entry["wrong_calls"] += 1
    for arm in out.values():
        for entry in arm.values():
            entry["rate"] = (entry["wrong_calls"] / entry["opportunities"]
                             if entry["opportunities"] else None)
    return out


# ----------------------------------------------------------------- decisions

def _mean(values):
    values = [v for v in values if v is not None]
    return sum(values) / len(values) if values else None


def classify_delta(baseline_rates, treatment_rates,
                   neighbor_precision_delta: float = 0.0) -> dict:
    """Rule the whole report rests on: is this gap bigger than the noise?

    `baseline_rates` / `treatment_rates` are per-rep rates. With fewer than two
    reps the spread is unknown, so NOTHING can be credited -- the honest answer
    is no_effect with spread=None, not a lift on a single sample.
    """
    base, treat = _mean(baseline_rates), _mean(treatment_rates)
    delta = None if base is None or treat is None else treat - base
    usable = [v for v in baseline_rates if v is not None]
    spread = (max(usable) - min(usable)) if len(usable) >= 2 else None
    regression = neighbor_precision_delta > (spread if spread is not None else 0.0)
    if delta is None or spread is None or delta <= spread:
        verdict = "no_effect"
    elif regression:
        verdict = "regression"
    else:
        verdict = "lift"
    if verdict == "no_effect" and regression and delta is not None and delta > 0:
        verdict = "regression"
    return {"baseline_mean": base, "treatment_mean": treat, "delta": delta,
            "spread": spread, "neighbor_precision_delta": neighbor_precision_delta,
            "neighbor_precision_regression": bool(regression and (delta or 0) > 0),
            "verdict": verdict}


def per_task_best_variant(per_rep_task_rates, tasks, per_rep_pressure,
                          baseline="a") -> dict:
    """For each task and model: which variant, if any, beats A beyond spread."""
    by_task = {t["id"]: t for t in tasks}
    models = sorted({key[2] for rep in per_rep_task_rates for key in rep})
    variants = sorted({key[1] for rep in per_rep_task_rates for key in rep})
    out: dict = {}
    for task_id, task in sorted(by_task.items()):
        tool = task["correct_tool"]
        out[task_id] = {}
        for model in models:
            base = [rep.get((task_id, baseline, model), {}).get("rate")
                    for rep in per_rep_task_rates]
            candidates = {}
            for variant in variants:
                if variant == baseline:
                    continue
                treat = [rep.get((task_id, variant, model), {}).get("rate")
                         for rep in per_rep_task_rates]
                base_pressure = _mean([rep.get((baseline, model), {}).get(tool, {}).get("rate")
                                       for rep in per_rep_pressure])
                var_pressure = _mean([rep.get((variant, model), {}).get(tool, {}).get("rate")
                                      for rep in per_rep_pressure])
                pressure_delta = ((var_pressure - base_pressure)
                                  if None not in (var_pressure, base_pressure) else 0.0)
                candidates[variant] = classify_delta(base, treat, pressure_delta)
            lifts = {v: c for v, c in candidates.items() if c["verdict"] == "lift"}
            best = max(lifts, key=lambda v: lifts[v]["delta"]) if lifts else baseline
            out[task_id][model] = {
                "best_variant": best,
                "verdict": lifts[best]["verdict"] if lifts else "no_effect",
                "baseline_mean": _mean(base),
                "candidates": candidates,
            }
    return out


# --------------------------------------------------------------- global claims

def _rate(cells, variant, model, tier):
    cell = cells.get((variant, model, tier))
    return cell["rate"] if cell else None


def evaluate_global_claims(per_rep_cells, per_rep_task_rates, per_rep_pressure,
                           best) -> dict:
    """Mechanically judge the registry's six global claims. Ties go to 'held: null'."""
    variants = sorted({k[0] for rep in per_rep_cells for k in rep})
    models = sorted({k[1] for rep in per_rep_cells for k in rep})
    claims: dict = {}

    # g1: does D kill the search<->callers confusion, both directions?
    callers_search = _mean([rep.get(("confirmed-call-sites-for-one-routine", "d", m), {})
                            .get("columns", {}).get("search", 0)
                            for rep in per_rep_task_rates for m in models])
    search_callers_a = _mean([rep.get(("where-does-this-text-appear", "a", m), {})
                              .get("columns", {}).get("callers", 0)
                              for rep in per_rep_task_rates for m in models])
    search_callers_d = _mean([rep.get(("where-does-this-text-appear", "d", m), {})
                              .get("columns", {}).get("callers", 0)
                              for rep in per_rep_task_rates for m in models])
    claims["g1-d-fixes-callers-search-confusion"] = {
        "held": bool(callers_search == 0 and (search_callers_d or 0) <= (search_callers_a or 0)),
        "observed": (f"under D, mean `search` first calls on the callers task = "
                     f"{callers_search}; `callers` first calls on the search task "
                     f"A={search_callers_a} -> D={search_callers_d}"),
    }

    # g2: does the weak tier gain more than the strong tier from the best arm?
    gains = {}
    for model in models:
        base = [_rate(rep, "a", model, "confusable") for rep in per_rep_cells]
        best_gain, best_variant = None, None
        for variant in variants:
            if variant == "a":
                continue
            treat = [_rate(rep, variant, model, "confusable") for rep in per_rep_cells]
            delta = classify_delta(base, treat)["delta"]
            if delta is not None and (best_gain is None or delta > best_gain):
                best_gain, best_variant = delta, variant
        gains[model] = {"best_variant": best_variant, "delta": best_gain}
    weak, strong = gains.get("weak", {}), gains.get("strong", {})
    claims["g2-weak-tier-gains-most"] = {
        "held": bool(weak.get("delta") is not None and strong.get("delta") is not None
                     and weak["delta"] > strong["delta"]),
        "observed": f"best-arm gain weak={weak.get('delta')} strong={strong.get('delta')}",
    }

    # g3: the control null.
    control = {}
    for model in models:
        base = [_rate(rep, "a", model, "control") for rep in per_rep_cells]
        for variant in variants:
            if variant == "a":
                continue
            treat = [_rate(rep, variant, model, "control") for rep in per_rep_cells]
            control[f"{variant}|{model}"] = classify_delta(base, treat)
    claims["g3-control-null"] = {
        "held": all(v["verdict"] == "no_effect" for v in control.values()) or not control,
        "observed": {k: v["verdict"] for k, v in sorted(control.items())},
    }

    # g4: native-first is wording-insensitive on the strong tier.
    natives = {}
    for variant in variants:
        share = []
        for rep in per_rep_cells:
            cell = rep.get((variant, "strong", "confusable"))
            if cell and cell["denominator"]:
                native = sum(v for k, v in cell["columns"].items()
                             if str(k).startswith("native:"))
                share.append(native / cell["denominator"])
        natives[variant] = _mean(share)
    base_native = natives.get("a")
    claims["g4-native-first-is-wording-insensitive"] = {
        "held": bool(base_native is not None and all(
            v is None or v >= base_native - 0.05 for k, v in natives.items() if k != "a")),
        "observed": {k: v for k, v in sorted(natives.items())},
    }

    # g5: C is a null.
    c_verdicts = {}
    for model in models:
        base = [_rate(rep, "a", model, "confusable") for rep in per_rep_cells]
        treat = [_rate(rep, "c", model, "confusable") for rep in per_rep_cells]
        c_verdicts[model] = classify_delta(base, treat)
    claims["g5-c-is-a-null"] = {
        "held": all(v["verdict"] == "no_effect" for v in c_verdicts.values()),
        "observed": {k: {"delta": v["delta"], "spread": v["spread"],
                         "verdict": v["verdict"]} for k, v in sorted(c_verdicts.items())},
    }

    # g6: hallucination stays rare.
    halluc = {}
    for rep in per_rep_cells:
        for key, cell in rep.items():
            attempted = cell["denominator"] + sum(cell["excluded"].values())
            if attempted:
                halluc.setdefault(f"{key[0]}|{key[1]}", []).append(
                    cell["excluded"].get("hallucinated_tool", 0) / attempted)
    halluc_means = {k: _mean(v) for k, v in sorted(halluc.items())}
    claims["g6-hallucination-rate-low"] = {
        "held": all(v is None or v < 0.05 for v in halluc_means.values()),
        "observed": halluc_means,
    }
    return claims


def diff_predictions(predictions, observed) -> dict:
    """Mechanical diff. Misses lead; a hit list alone proves nothing."""
    hits, misses = [], []
    for claim in predictions.get("global_predictions", []):
        result = observed.get("global_claims", {}).get(claim["id"])
        if result is None:
            misses.append({"id": claim["id"], "claim": claim["claim"],
                           "observed": "not evaluable from the ledgers",
                           "hypothesis": "the grid did not produce the cells this "
                                         "claim needs; treat as untested, not as held"})
        elif result["held"]:
            hits.append({"id": claim["id"], "observed": result["observed"]})
        else:
            misses.append({"id": claim["id"], "claim": claim["claim"],
                           "observed": result["observed"],
                           "hypothesis": result.get("hypothesis")
                           or "see the named hypothesis in the Markdown analysis"})
    for prediction in predictions.get("per_task", []):
        task_id = prediction["task_id"]
        seen = observed.get("per_task_best_variant", {}).get(task_id)
        if seen is None:
            continue
        predicted = prediction.get("best_variant")
        actual = sorted({m["best_variant"] for m in seen.values()})
        if predicted in ("none", None):
            held = all(m["verdict"] == "no_effect" for m in seen.values())
        else:
            held = any(m["best_variant"] == predicted for m in seen.values())
        entry = {"id": f"per_task:{task_id}", "predicted": predicted,
                 "observed": f"best variant per model: "
                             + ", ".join(f"{m}={v['best_variant']}({v['verdict']})"
                                         for m, v in sorted(seen.items()))}
        if held:
            hits.append(entry)
        else:
            entry["claim"] = f"{task_id}: best variant {predicted}"
            entry["hypothesis"] = ("predicted variant did not clear the spread; the "
                                   f"arms that did were {actual}")
            misses.append(entry)
    return {"misses": misses, "hits": hits,
            "summary": {"misses": len(misses), "hits": len(hits)}}


# -------------------------------------------------------------------- report

def build_report(ledger_paths, tasks, predictions,
                 predictions_commit_time: str, excluded_ledgers=None) -> dict:
    ledgers = [load_ledger(p) for p in sorted(ledger_paths)]
    problems = ordering_problems(predictions_commit_time, ledgers)
    if problems:
        raise OrderingError("; ".join(problems))

    per_rep_cells = [cell_rates(l["records"]) for l in ledgers]
    per_rep_tasks = [task_rates(l["records"]) for l in ledgers]
    per_rep_pressure = [neighbor_pressure(l["records"], tasks) for l in ledgers]
    all_records = [r for l in ledgers for r in l["records"]]

    best = per_task_best_variant(per_rep_tasks, tasks, per_rep_pressure)
    observed = {
        "per_task_best_variant": best,
        "global_claims": evaluate_global_claims(per_rep_cells, per_rep_tasks,
                                                per_rep_pressure, best),
    }

    failures: dict = defaultdict(int)
    for record in all_records:
        if not record.get("counts_in_matrix"):
            failures[record.get("outcome", "unknown")] += 1

    control_null = {}
    for tier in ("control",):
        for key in sorted({k for rep in per_rep_cells for k in rep if k[2] == tier}):
            control_null.setdefault(tier, {})[f"{key[0]}|{key[1]}"] = {
                "rate": _mean([rep.get(key, {}).get("rate") for rep in per_rep_cells]),
                "n": sum(rep.get(key, {}).get("denominator", 0) for rep in per_rep_cells),
            }

    def key_str(key):
        return "|".join(key)

    return {
        "schema": "toolcalling_selection_analysis_v1",
        "version": 1,
        "boundary": BOUNDARY,
        "caveats": CAVEATS,
        "reps": len(ledgers),
        "ledgers": [{"path": Path(l["path"]).name, "started_at": l["started_at"],
                     "records": len(l["records"])} for l in ledgers],
        "excluded_ledgers": excluded_ledgers or [],
        "predictions_commit_time": predictions_commit_time,
        "records": len(all_records),
        "confusion_matrices": [
            {"cell": key_str(key),
             "per_rep": [{"denominator": rep.get(key, {}).get("denominator", 0),
                          "correct": rep.get(key, {}).get("correct", 0),
                          "rate": rep.get(key, {}).get("rate"),
                          "columns": rep.get(key, {}).get("columns", {}),
                          "excluded": rep.get(key, {}).get("excluded", {})}
                         for rep in per_rep_cells],
             "mean_rate": _mean([rep.get(key, {}).get("rate") for rep in per_rep_cells])}
            for key in sorted({k for rep in per_rep_cells for k in rep})],
        "neighbor_pressure": {
            key_str(key): {tool: _mean([rep.get(key, {}).get(tool, {}).get("rate")
                                        for rep in per_rep_pressure])
                           for tool in sorted({t for rep in per_rep_pressure
                                               for t in rep.get(key, {})})}
            for key in sorted({k for rep in per_rep_pressure for k in rep})},
        "per_task_best_variant": best,
        "global_claims": observed["global_claims"],
        "control_null": control_null,
        "provider_failures": dict(sorted(failures.items())),
        "prediction_diff": diff_predictions(predictions, observed),
        "inputs_digest": hashlib.sha256(
            "".join(sorted(json.dumps(r, sort_keys=True) for r in all_records)
                    ).encode()).hexdigest()[:16],
    }


# ----------------------------------------------------------------------- CLI

def load_tasks(tasks_dir) -> list[dict]:
    return [json.loads(p.read_text()) for p in sorted(Path(tasks_dir).glob("*.json"))]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ledgers", nargs="+", required=True,
                        help="one JSONL ledger per rep (or per shard; shards of the "
                             "same rep are merged by passing them as one --rep-glob)")
    parser.add_argument("--tasks-dir", default=TASKS_DIR)
    parser.add_argument("--predictions", default=PREDICTIONS)
    parser.add_argument("--predictions-commit-time", required=True,
                        help="committer time of the predictions.json commit, ISO-8601 UTC")
    parser.add_argument("--exclude-ledger", nargs="*", default=[],
                        help="path plus reason, as path=reason; recorded in the report")
    parser.add_argument("--out", default=DEFAULT_OUT / "selection-analysis.json")
    args = parser.parse_args(argv)

    excluded = [{"path": item.split("=", 1)[0], "reason": item.split("=", 1)[1]}
                for item in args.exclude_ledger if "=" in item]
    report = build_report(args.ledgers, load_tasks(args.tasks_dir),
                          json.loads(Path(args.predictions).read_text()),
                          args.predictions_commit_time, excluded_ledgers=excluded)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(f"wrote {out} ({report['records']} records, {report['reps']} reps, "
          f"{report['prediction_diff']['summary']['misses']} prediction misses)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
