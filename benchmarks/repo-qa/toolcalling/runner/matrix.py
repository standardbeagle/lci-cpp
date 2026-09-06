"""Aggregate graded records into a confusion matrix per (variant, model, tier).

A hit rate cannot be read against the confusable-neighbour control: the point
of the bank is WHICH wrong tool a model reaches for, so the unit of output is
rows (the task's correct tool) by columns (the tool actually called first).

Tiers are never pooled. A control task is single-tool-obvious by construction,
so mixing it into the confusable denominator inflates every arm equally and
hides the delta the epic is looking for.
"""

from __future__ import annotations

from collections import defaultdict


def confusion_matrices(records) -> list[dict]:
    groups: dict[tuple, dict] = {}
    for record in records:
        key = (record["variant"], record["model"], record["tier"])
        cell = groups.setdefault(key, {
            "variant": key[0], "model": key[1], "tier": key[2],
            "rows": defaultdict(lambda: defaultdict(int)),
            "denominator": 0,
            "excluded": defaultdict(int),
        })
        if record.get("counts_in_matrix") and record.get("matrix_cell"):
            row, column = record["matrix_cell"]
            cell["rows"][row][column] += 1
            cell["denominator"] += 1
        else:
            cell["excluded"][record.get("outcome", "unknown")] += 1

    out = []
    for key in sorted(groups):
        cell = groups[key]
        rows = {row: dict(columns) for row, columns in cell["rows"].items()}
        correct = sum(columns.get(row, 0) for row, columns in rows.items())
        out.append({
            "variant": cell["variant"], "model": cell["model"],
            "tier": cell["tier"],
            "rows": rows,
            "denominator": cell["denominator"],
            "correct_first_call": correct,
            "excluded": dict(cell["excluded"]),
        })
    return out
