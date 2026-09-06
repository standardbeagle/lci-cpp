#!/usr/bin/env python3
"""Turn D4's sweep rows into a separation verdict and a prediction diff.

Reads one or more completed sweep run directories (the resume-safe row files
`discovery/runner/sweep.py` writes) plus the pre-registered
`discovery/predictions.json`, and emits `discovery_analysis_v1` JSON.

Three rules are load-bearing, and each has a discrimination test in
`tests/test_discovery_analysis.py`:

  1. **A gap inside the run-to-run spread is not separation.** Separation is
     claimed only when |mean delta| exceeds the spread of that delta ACROSS
     independent runs, and never from a single run (spread unmeasured). Prior
     tiers read 0.96 vs 0.96 as a result; this refuses to.
  2. **A family that never ran is NOT RUN, never a zero.** It carries its
     reason, contributes to no aggregate, and cannot become a prediction miss.
  3. **Tool level and agent level are never collapsed.** Tool level answers
     "is LCI correct"; agent level answers "does an agent gain from it". A
     family correct at the tool level with no agent lift is an ADOPTION gap,
     named separately from a capability gap.

DNF is a rate of its own, never folded into the accuracy means as a zero:
"never answered" and "answered wrongly" are different failures.

    analyze_discovery.py --runs .work/discovery/tool-run1,.work/discovery/tool-run2 \
        --out discovery/analysis
"""

import argparse
import json
import math
import os
import statistics
import sys

SCHEMA = "discovery_analysis_v1"

# Prediction vocabulary from predictions.json, mapped onto what a run can show.
OUTCOME_LCI = "lci_wins"
OUTCOME_GREP = "grep_wins"
OUTCOME_PARITY = "parity"

_METRIC_KEYS = {"mean_f1": "f1", "mean_precision": "precision",
                "mean_recall": "recall"}
DNF_METRIC = "dnf_rate_pct"

# Same bar D4 uses: a treatment this accurate through its own surface HAS the
# capability, so a missing agent-level lift is adoption, not capability.
TOOL_CORRECT_F1 = 0.8

# Call-site-noise floors reported as "wins above noise ratio N". 2.0 and 8.0
# are D2's OWN published cohort thresholds, carried over rather than retuned;
# the rest bracket them so the shape of the curve is visible.
NOISE_FLOORS = (1.0, 2.0, 4.0, 8.0, 16.0, 32.0)


def _is_row_file(name):
    return name.endswith(".json") and name.count("__") == 3


def load_run(run_dir):
    rows = []
    for name in sorted(os.listdir(run_dir)):
        if not _is_row_file(name):
            continue
        with open(os.path.join(run_dir, name), encoding="utf-8") as handle:
            rows.append(json.load(handle))
    return rows


def _mean(values):
    return round(sum(values) / len(values), 6) if values else None


def _spread(values):
    values = [v for v in values if v is not None]
    if len(values) < 2:
        return None
    return round(max(values) - min(values), 6)


def _stdev(values):
    values = [v for v in values if v is not None]
    if len(values) < 2:
        return None
    return round(statistics.stdev(values), 6)


def _arm_summary(rows):
    """Per-arm accuracy means with DNF held out as its own rate."""
    total = len(rows)
    dnf = [r for r in rows if r["status"] == "dnf"]
    graded = [r for r in rows
              if r["status"] == "ok" and r["score"].get("f1") is not None]
    summary = {
        "cell_count": total,
        "graded_count": len(graded),
        "dnf_count": len(dnf),
        "dnf_rate_pct": round(100.0 * len(dnf) / total, 4) if total else None,
        "correct_count": sum(1 for r in graded if r["score"].get("correct")),
        "zero_citation_count": sum(1 for r in graded
                                   if r["score"].get("cited_total") == 0),
    }
    for name, key in _METRIC_KEYS.items():
        summary[name] = _mean([r["score"][key] for r in graded])
    for name, key in (("mean_tool_calls", "tool_calls"), ("mean_tokens", "tokens"),
                      ("mean_wall_seconds", "wall_seconds")):
        values = [r[key] for r in rows if r.get(key) is not None]
        summary[name] = _mean(values)
    return summary


def _signed_delta(metric, arms):
    """treatment - baseline, in the metric's own units. None if either is absent."""
    if metric == DNF_METRIC:
        # Lower DNF is better, so the treatment-positive direction is inverted.
        t = arms.get("treatment", {}).get("dnf_rate_pct")
        b = arms.get("baseline", {}).get("dnf_rate_pct")
        return None if t is None or b is None else round(b - t, 6)
    t = arms.get("treatment", {}).get(metric)
    b = arms.get("baseline", {}).get(metric)
    return None if t is None or b is None else round(t - b, 6)


def _threshold_metric(threshold):
    if not threshold:
        return "mean_f1"
    metric = threshold.get("metric", "mean_f1")
    return metric if metric in _METRIC_KEYS or metric == DNF_METRIC else "mean_f1"


def _threshold_met(threshold, delta):
    """Does the SIGNED treatment-positive delta clear the registered bar?"""
    if not threshold or delta is None:
        return None
    value = threshold.get("value")
    comparison = threshold.get("comparison", "treatment_minus_baseline")
    operator = threshold.get("operator", ">=")
    if comparison == "abs_treatment_minus_baseline":
        observed = abs(delta)
    elif comparison == "baseline_minus_treatment":
        observed = -delta
    else:
        observed = delta
    if operator == ">=":
        return observed >= value
    if operator == "<=":
        return observed <= value
    if operator == ">":
        return observed > value
    if operator == "<":
        return observed < value
    return None


def _binom_two_sided(wins, n):
    """Exact two-sided sign-test p under p=0.5. Small n, so no approximation."""
    if n == 0:
        return None
    k = min(wins, n - wins)
    tail = sum(math.comb(n, i) for i in range(0, k + 1)) / float(2 ** n)
    return round(min(1.0, 2 * tail), 6)


def _sign_test(per_cell):
    """Discordant-pair sign test over per-cell treatment-minus-baseline deltas."""
    discordant = [d for d in per_cell.values() if d is not None and d != 0.0]
    wins = sum(1 for d in discordant if d > 0)
    return {
        "cells": len(per_cell),
        "discordant": len(discordant),
        "treatment_wins": wins,
        "baseline_wins": len(discordant) - wins,
        "ties": len(per_cell) - len(discordant),
        "two_sided_p": _binom_two_sided(wins, len(discordant)),
        "unanimous": bool(discordant) and (wins == len(discordant)
                                           or wins == 0),
    }


def _per_cell_deltas(level_rows, metric):
    """Mean per-cell delta across runs, keyed by slug."""
    key = _METRIC_KEYS.get(metric, "f1")
    per_run = {}
    for run_name, rows in level_rows.items():
        by_slug = {}
        for row in rows:
            if row["status"] != "ok" or row["score"].get(key) is None:
                continue
            by_slug.setdefault(row["slug"], {})[row["arm"]] = row["score"][key]
        for slug, arms in by_slug.items():
            if "treatment" in arms and "baseline" in arms:
                per_run.setdefault(slug, []).append(arms["treatment"] - arms["baseline"])
    return {slug: _mean(deltas) for slug, deltas in per_run.items()}


def _analyze_level(level_rows_by_run, threshold):
    """One family at one level, across every run. Rules 1 and 3 live here."""
    metric = _threshold_metric(threshold)
    per_run_arms = {}
    per_run_delta = {}
    for run_name, rows in level_rows_by_run.items():
        arms = {}
        for arm in ("treatment", "baseline"):
            arm_rows = [r for r in rows if r["arm"] == arm]
            if arm_rows:
                arms[arm] = _arm_summary(arm_rows)
        per_run_arms[run_name] = arms
        per_run_delta[run_name] = _signed_delta(metric, arms)

    run_names = sorted(per_run_arms)
    deltas = [per_run_delta[n] for n in run_names if per_run_delta[n] is not None]
    delta_mean = _mean(deltas)
    delta_spread = _spread(deltas)
    variance_unmeasured = len(deltas) < 2

    # RULE 1: a gap no larger than the run-to-run spread is noise, not a result.
    within_variance = (
        False if variance_unmeasured or delta_mean is None
        else abs(delta_mean) <= delta_spread
    )
    threshold_met = _threshold_met(threshold, delta_mean)
    claimable = (not variance_unmeasured) and (not within_variance)
    separation = "none"
    if claimable and delta_mean is not None and delta_mean != 0.0:
        # A parity threshold registers a bar the delta must stay UNDER; a
        # directional one registers a bar it must clear. Either way separation
        # needs the magnitude bar met, not just a sign.
        directional = (threshold or {}).get("comparison") != "abs_treatment_minus_baseline"
        if directional and threshold_met is False:
            separation = "none"
        elif not directional and threshold_met:
            separation = "none"  # inside the registered parity band
        else:
            separation = "treatment" if delta_mean > 0 else "baseline"

    # Aggregate arms across runs, keeping each run's own number visible.
    arms_across = {}
    for arm in ("treatment", "baseline"):
        present = [per_run_arms[n][arm] for n in run_names if arm in per_run_arms[n]]
        if not present:
            continue
        block = {"runs": len(present)}
        for name in list(_METRIC_KEYS) + ["dnf_rate_pct", "mean_tool_calls",
                                          "mean_tokens", "mean_wall_seconds"]:
            values = [p[name] for p in present if p.get(name) is not None]
            block[name] = _mean(values)
            block[name + "_spread"] = _spread(values)
        block["per_run"] = {n: per_run_arms[n][arm] for n in run_names
                            if arm in per_run_arms[n]}
        arms_across[arm] = block

    per_cell = _per_cell_deltas(level_rows_by_run, metric)
    observed = sorted({row["slug"] for rows in level_rows_by_run.values()
                       for row in rows})
    return {
        "cells_observed": len(observed),
        "slugs_observed": observed,
        "metric": metric,
        "threshold_registered": bool(threshold),
        "runs": run_names,
        "run_count": len(run_names),
        "arms": arms_across,
        "delta_per_run": {n: per_run_delta[n] for n in run_names},
        "delta_mean": delta_mean,
        "delta_spread": delta_spread,
        "delta_stdev": _stdev(deltas),
        "variance_unmeasured": variance_unmeasured,
        "within_variance": within_variance,
        "threshold_met": threshold_met,
        "separation": separation,
        "separation_claimable": claimable,
        "per_cell_delta": per_cell,
        "sign_test": _sign_test(per_cell),
    }


def _observed_outcome(level):
    if level["separation"] == "treatment":
        return OUTCOME_LCI
    if level["separation"] == "baseline":
        return OUTCOME_GREP
    return OUTCOME_PARITY


def _difficulty_correlation(families, cohorts):
    """Per-cell treatment delta joined onto D2's difficulty axes.

    The headline the epic asked for is "LCI wins ABOVE noise ratio N", so the
    buckets are cumulative floors, not disjoint bins.
    """
    by_slug = {s["slug"]: s for s in cohorts.get("symbols", [])}
    points = []
    for fid, family in families.items():
        for level_name, level in family.get("levels", {}).items():
            for slug, delta in level.get("per_cell_delta", {}).items():
                symbol = by_slug.get(slug)
                if symbol is None or delta is None:
                    continue
                fan_in = symbol.get("fan_in") or 0
                noise = (symbol["grep_hit_count"] / fan_in) if fan_in else None
                points.append({
                    "family": fid, "level": level_name, "slug": slug,
                    "delta": delta, "call_site_noise": (round(noise, 4)
                                                        if noise is not None else None),
                    "def_count": symbol.get("def_count"),
                    "fan_in": fan_in,
                    "noise_ratio": symbol.get("noise_ratio"),
                })
    buckets = []
    for floor in NOISE_FLOORS:
        inside = [p for p in points
                  if p["call_site_noise"] is not None and p["call_site_noise"] >= floor]
        buckets.append({
            "floor": floor,
            "cells": len(inside),
            "mean_delta": _mean([p["delta"] for p in inside]),
            "treatment_win_rate": (round(sum(1 for p in inside if p["delta"] > 0)
                                         / len(inside), 6) if inside else None),
        })
    return {
        "call_site_noise": {"buckets": buckets,
                            "note": "cumulative floors; 2.0 and 8.0 are D2's own "
                                    "published control/high cohort thresholds"},
        "points": sorted(points, key=lambda p: (p["family"], p["level"], p["slug"])),
    }


def analyze(registry, cohorts, run_dirs, not_run_reasons=None):
    not_run_reasons = dict(not_run_reasons or {})
    runs = {}
    for run_dir in run_dirs:
        runs[os.path.basename(os.path.normpath(run_dir))] = load_run(run_dir)

    by_family = {}
    for run_name, rows in runs.items():
        for row in rows:
            by_family.setdefault(row["family"], {}) \
                     .setdefault(row["level"], {}) \
                     .setdefault(run_name, []).append(row)

    families = {}
    not_run = []
    misses = []
    adoption_gap = []
    unmeasurable = []

    for entry in registry["families"]:
        fid = entry["id"]
        prediction = entry.get("prediction") or {}
        predicted = prediction.get("outcome")
        record = {
            "role": entry.get("role"),
            "task_shape": entry.get("task_shape"),
            "declared_n": entry.get("cohort", {}).get("n"),
            "threshold": entry.get("threshold"),
            "predicted_outcome": predicted,
            "prediction_confidence": prediction.get("confidence"),
            "epic_intuition": (entry.get("epic_intuition") or {}).get("outcome"),
        }

        # RULE 2: never ran => NOT RUN, with its reason, and no aggregates.
        if fid not in by_family:
            record.update({
                "status": "not_run",
                "not_run_reason": not_run_reasons.get(
                    fid, "no rows in any supplied run directory"),
                "levels": {},
                "observed_outcome": None,
                "prediction_held": None,
            })
            families[fid] = record
            not_run.append({"family": fid, "reason": record["not_run_reason"]})
            continue

        levels = {}
        for level_name, per_run in by_family[fid].items():
            levels[level_name] = _analyze_level(per_run, entry.get("threshold"))
        observed = {name: _observed_outcome(level) for name, level in levels.items()}

        held = {}
        for name, outcome in observed.items():
            if not predicted or predicted not in (OUTCOME_LCI, OUTCOME_GREP,
                                                  OUTCOME_PARITY):
                held[name] = None
                continue
            held[name] = (outcome == predicted)
            if held[name] is False:
                misses.append({
                    "family": fid, "level": name, "predicted": predicted,
                    "observed": outcome,
                    "delta_mean": levels[name]["delta_mean"],
                    "delta_spread": levels[name]["delta_spread"],
                    "metric": levels[name]["metric"],
                    "confidence": prediction.get("confidence"),
                    "registered_mechanism": prediction.get("mechanism"),
                })

        # RULE 3: capability (tool) and adoption (agent) stay separate.
        tool = levels.get("tool")
        agent = levels.get("agent")
        if tool and tool["arms"].get("treatment", {}).get("mean_f1") is not None:
            tool_f1 = tool["arms"]["treatment"]["mean_f1"]
            if tool_f1 >= TOOL_CORRECT_F1 and tool["separation"] == "treatment":
                if agent is None:
                    unmeasurable.append(fid)
                elif agent["separation"] != "treatment":
                    adoption_gap.append(fid)

        record.update({
            "status": "analyzed",
            "not_run_reason": None,
            "levels": levels,
            "observed_outcome": observed,
            "prediction_held": held,
        })
        families[fid] = record

    return {
        "schema_version": SCHEMA,
        "registry_id": registry.get("registry_id"),
        "corpus_commit": cohorts.get("corpus_commit"),
        "runs": sorted(runs),
        "run_count": len(runs),
        "families": families,
        "not_run": sorted(not_run, key=lambda e: e["family"]),
        "prediction_misses": sorted(misses, key=lambda m: (m["family"], m["level"])),
        "tool_correct_no_agent_lift": sorted(adoption_gap),
        "tool_correct_agent_level_unmeasurable": sorted(unmeasurable),
        "difficulty_correlation": _difficulty_correlation(families, cohorts),
    }


def _load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def main(argv=None):
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--registry",
                        default=os.path.join(here, "discovery", "predictions.json"))
    parser.add_argument("--cohorts",
                        default=os.path.join(here, "discovery", "cohorts",
                                             "cohorts.json"))
    parser.add_argument("--runs", required=True,
                        help="comma-separated sweep run directories")
    parser.add_argument("--not-run", action="append", default=[],
                        metavar="FAMILY=REASON",
                        help="a family that could not be run, and why; recorded "
                             "as NOT RUN instead of a zero row")
    parser.add_argument("--out", help="directory to write analysis.json into")
    args = parser.parse_args(argv)

    reasons = {}
    for item in args.not_run:
        fid, _, reason = item.partition("=")
        reasons[fid] = reason or "not stated"

    result = analyze(_load(args.registry), _load(args.cohorts),
                     [r for r in args.runs.split(",") if r], reasons)
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        path = os.path.join(args.out, "analysis.json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(result, handle, indent=2, sort_keys=True)
        print("analysis: %s" % path)
    else:
        json.dump(result, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
