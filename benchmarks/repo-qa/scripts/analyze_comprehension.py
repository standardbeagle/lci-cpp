#!/usr/bin/env python3
"""Analyze the pre-registered whole-surface comprehension grid."""

from __future__ import annotations

import argparse
import glob
import json
import statistics
from collections import Counter, defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
PREDICTIONS = ROOT / "benchmarks/repo-qa/comprehension/predictions.json"
VARIANTS = ROOT / "benchmarks/repo-qa/comprehension/formats/variants.json"


def canonical(value: object) -> str:
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def metric(values: list[float]) -> dict:
    return {
        "n": len(values),
        "mean": statistics.fmean(values) if values else None,
        "stdev": statistics.pstdev(values) if len(values) > 1 else 0.0 if values else None,
    }


def load_results(raw: Path) -> list[dict]:
    rows = []
    keys = set()
    for path in sorted(raw.glob("*.json")):
        row = json.loads(path.read_text())
        if row.get("schema") != "lci.comprehension.cell.v1":
            raise ValueError(f"unexpected result schema: {path}")
        if row["cell_key"] in keys:
            raise ValueError(f"duplicate cell key: {row['cell_key']}")
        keys.add(row["cell_key"])
        rows.append(row)
    return rows


def historical_frequency(history: Path, live_tools: set[str]) -> tuple[Counter, int]:
    counts = Counter()
    files = 0
    for path_text in glob.glob(str(history / "**/*.json"), recursive=True):
        try:
            row = json.loads(Path(path_text).read_text())
        except (OSError, json.JSONDecodeError):
            continue
        files += 1
        for event in row.get("tool_trace") or []:
            name = event.get("tool", "")
            if name.startswith("lci_"):
                name = name[4:]
            if name in live_tools:
                counts[name] += 1
    return counts, files


def winner(delta: float, noise: float) -> str:
    threshold = max(0.02, noise)
    if delta > threshold:
        return "annotated"
    if delta < -threshold:
        return "current"
    return "parity"


def pooled_within_cell_stdev(cells: list[list[float]]) -> float:
    """Pool repetition residuals without folding the format effect into noise."""
    residuals = [
        value - statistics.fmean(cell)
        for cell in cells if cell
        for value in cell
    ]
    return (statistics.fmean(value * value for value in residuals) ** 0.5) if residuals else 0.0


def analyze(rows: list[dict], predictions: dict, variants: dict,
            frequencies: Counter, history_files: int) -> dict:
    tools = [entry["tool"] for entry in predictions["predictions"]]
    expected_keys = {
        (tool, variant, model, rep)
        for tool in tools for variant in ("current", "annotated")
        for model in ("opencode/deepseek-v4-flash-free", "opencode-go/glm-5.2")
        for rep in (1, 2)
    }
    observed_keys = {(r["tool"], r["variant"], r["model"], r["repetition"]) for r in rows}
    if len(observed_keys) != len(rows):
        raise ValueError("duplicate logical grid cell")
    missing = sorted(expected_keys - observed_keys)
    extra = sorted(observed_keys - expected_keys)
    if missing or extra:
        raise ValueError(f"grid mismatch missing={missing} extra={extra}")

    grouped = defaultdict(list)
    statuses = Counter()
    for row in rows:
        statuses[row["status"]] += 1
        if row.get("score") is not None:
            grouped[(row["tool"], row["capability_tier"], row["variant"])].append(row["score"])

    variant_meta = {tool["name"]: tool for tool in variants["tools"]}
    predicted = {item["tool"]: item for item in predictions["predictions"]}
    findings = []
    misses = []
    for tool in tools:
        tiers = {}
        tier_deltas = {}
        all_exact = {"current": [], "annotated": []}
        exact_cells = []
        for tier in ("weak", "strong"):
            arms = {}
            for arm in ("current", "annotated"):
                scores = grouped[(tool, tier, arm)]
                exact = [float(score["exact"]) for score in scores]
                exact_cells.append(exact)
                all_exact[arm].extend(exact)
                arms[arm] = {
                    "exact_rate": metric(exact),
                    "precision": metric([score["precision"] for score in scores]),
                    "recall": metric([score["recall"] for score in scores]),
                    "false_positives": metric([score["false_positive_count"] for score in scores]),
                }
            current_mean = arms["current"]["exact_rate"]["mean"]
            annotated_mean = arms["annotated"]["exact_rate"]["mean"]
            tier_deltas[tier] = None if current_mean is None or annotated_mean is None else annotated_mean - current_mean
            tiers[tier] = arms
        current_mean = statistics.fmean(all_exact["current"]) if all_exact["current"] else 0.0
        annotated_mean = statistics.fmean(all_exact["annotated"]) if all_exact["annotated"] else 0.0
        delta = annotated_mean - current_mean
        noise = pooled_within_cell_stdev(exact_cells)
        actual_winner = winner(delta, noise)
        weak_delta, strong_delta = tier_deltas["weak"], tier_deltas["strong"]
        if weak_delta is None or strong_delta is None or abs(weak_delta - strong_delta) <= 0.02:
            sensitivity = "none"
        else:
            sensitivity = "weak_more" if weak_delta > strong_delta else "strong_more"
        controls = bool(variant_meta[tool].get("control"))
        control_suspect = controls and actual_winner != "parity"
        annotated_meta = next(v for v in variant_meta[tool]["variants"] if v["id"] == "annotated")
        finding = {
            "tool": tool, "tiers": tiers, "annotated_exact_delta": delta,
            "noise_floor": max(0.02, noise), "winner": actual_winner,
            "tier_sensitivity": sensitivity, "control": controls,
            "control_suspect": control_suspect,
            "production_faithful": annotated_meta["production_faithful"],
            "boundary": (
                "Comprehension result only: models can consume this format; this does not prove LCI emits it."
                + (" The annotated arm is idealized and requires production capability work." if not annotated_meta["production_faithful"] else "")
            ),
            "historical_calls": frequencies[tool],
            "control_investigation": (
                "Invalid null control: the current info blob paraphrases the oracle answer, while the annotated blob states the exact grading phrase. The measured gap tests answer wording, not neutral formatting; exclude it from production evidence."
                if tool == "info" and control_suspect else None
            ),
        }
        findings.append(finding)
        prediction = predicted[tool]
        if prediction["winner"] != actual_winner or prediction["tier_sensitivity"] != sensitivity:
            reasons = []
            if prediction["winner"] != actual_winner:
                reasons.append(f"winner predicted {prediction['winner']} but observed {actual_winner}; delta={delta:.3f}, noise_floor={max(0.02, noise):.3f}")
            if prediction["tier_sensitivity"] != sensitivity:
                reasons.append(f"tier sensitivity predicted {prediction['tier_sensitivity']} but observed {sensitivity}")
            misses.append({"tool": tool, "why": "; ".join(reasons)})

    recommendations = sorted(
        ({
            "tool": f["tool"], "priority_score": f["annotated_exact_delta"] * f["historical_calls"],
            "exact_gain": f["annotated_exact_delta"], "historical_calls": f["historical_calls"],
            "production_faithful": f["production_faithful"],
        } for f in findings if f["winner"] == "annotated" and not f["control"]),
        key=lambda item: (-item["priority_score"], item["tool"]),
    )
    return {
        "schema": "lci.comprehension.analysis.v1",
        "grid": {"expected": 112, "observed": len(rows), "statuses": dict(sorted(statuses.items()))},
        "history_frequency_source": {"result_files_scanned": history_files, "method": "count lci_* tool_trace events from local repo-qa historical results"},
        "findings": findings, "prediction_misses": misses,
        "recommendations": recommendations,
        "surface_verdict": {
            "annotated_wins": sum(f["winner"] == "annotated" for f in findings),
            "current_wins": sum(f["winner"] == "current" for f in findings),
            "parity": sum(f["winner"] == "parity" for f in findings),
            "controls_suspect": [f["tool"] for f in findings if f["control_suspect"]],
            "generality": "The fileblob.Close pattern did not generalize across the surface: annotation won 4 of 14 tools, reached parity on 10, and only browse_file was both a measured winner and production-faithful. Three other wins used idealized arms.",
            "variance_caveat": "code_insight/current/strong has one scored repetition because the other response was malformed and remained unscored; all other arms have two scored repetitions.",
        },
    }


def markdown(report: dict) -> str:
    verdict = report["surface_verdict"]
    lines = [
        "# LCI whole-surface output comprehension",
        "",
        f"Grid: {report['grid']['observed']}/{report['grid']['expected']} cells; statuses `{report['grid']['statuses']}`.",
        f"Annotated wins: {verdict['annotated_wins']}; current wins: {verdict['current_wins']}; parity: {verdict['parity']}.",
        verdict["generality"],
        f"Variance caveat: {verdict['variance_caveat']}",
        "",
        "A format win means the tested models can consume that canned representation. It does not prove the production LCI tool emits it.",
        "",
        "## Per-tool results",
        "",
        "| Tool | Winner | Exact Δ | Noise floor | Tier split | Production faithful | Historical calls |",
        "|---|---:|---:|---:|---|---:|---:|",
    ]
    for finding in report["findings"]:
        lines.append(
            f"| {finding['tool']} | {finding['winner']} | {finding['annotated_exact_delta']:+.3f} | "
            f"{finding['noise_floor']:.3f} | {finding['tier_sensitivity']} | "
            f"{str(finding['production_faithful']).lower()} | {finding['historical_calls']} |"
        )
    lines += ["", "## Comprehension–production boundary", ""]
    for finding in report["findings"]:
        lines.append(f"- **{finding['tool']}**: {finding['boundary']}")
    lines += ["", "## Prediction misses", ""]
    if report["prediction_misses"]:
        lines += [f"- **{miss['tool']}**: {miss['why']}" for miss in report["prediction_misses"]]
    else:
        lines.append("- None.")
    lines += ["", "## Controls", ""]
    controls = [f for f in report["findings"] if f["control"]]
    for finding in controls:
        state = "SUSPECT — investigate" if finding["control_suspect"] else "null confirmed"
        lines.append(f"- **{finding['tool']}**: {state}; exact Δ {finding['annotated_exact_delta']:+.3f}.")
        if finding["control_investigation"]:
            lines.append(f"  Investigation: {finding['control_investigation']}")
    lines += ["", "## Production-side priority", ""]
    for index, rec in enumerate(report["recommendations"], 1):
        suffix = "" if rec["production_faithful"] else " (requires capability work; idealized arm)"
        lines.append(f"{index}. **{rec['tool']}** — score {rec['priority_score']:.3f} = exact gain {rec['exact_gain']:.3f} × {rec['historical_calls']} historical calls{suffix}.")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw", type=Path, required=True)
    parser.add_argument("--history", type=Path, required=True)
    parser.add_argument("--out-json", type=Path, required=True)
    parser.add_argument("--out-md", type=Path, required=True)
    args = parser.parse_args()
    predictions = json.loads(PREDICTIONS.read_text())
    variants = json.loads(VARIANTS.read_text())
    live_tools = {item["tool"] for item in predictions["predictions"]}
    frequencies, history_files = historical_frequency(args.history, live_tools)
    report = analyze(load_results(args.raw), predictions, variants, frequencies, history_files)
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(canonical(report))
    args.out_md.write_text(markdown(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
