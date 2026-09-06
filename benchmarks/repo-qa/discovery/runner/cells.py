"""Expand the pre-registered family registry into runnable cells.

Cells are derived from the committed cohort file at run time, never hard-coded,
so D2b's larger key pool runs through the same code once its predictions are
re-registered against it.

Two fail-loud gates guard the pre-registration:

  * a family whose derived cell count differs from its DECLARED n raises. The
    registry states n per family and D5 reads power off that n, so a silent
    drift between the file and the run would invalidate every power claim.
  * shrinking the grid is a knob (`max_cells_per_family`) that is RECORDED in
    the plan, per family, with the dropped slugs named. Truncating a family
    without it is the no-silent-caps violation the task forbids.
"""

import re


class PlanError(RuntimeError):
    """The registry and the cohort file disagree; refuse to run a wrong grid."""


_OPERATORS = {
    ">=": lambda a, b: a >= b,
    "<=": lambda a, b: a <= b,
    ">": lambda a, b: a > b,
    "<": lambda a, b: a < b,
}


def _by_slug(cohorts):
    return {s["slug"]: s for s in cohorts["symbols"]}


def _select_slugs(family, cohorts):
    selector = family["cohort"]["selector"]
    kind = selector["kind"]
    symbols = cohorts["symbols"]

    if kind == "cohort_cell":
        cell = selector["cell"]
        if cell not in cohorts["cohorts"]:
            raise PlanError(
                "family %s selects cohort cell %r, absent from the cohort file"
                % (family["id"], cell)
            )
        return sorted(cohorts["cohorts"][cell])

    if kind == "full_pool":
        return sorted(s["slug"] for s in symbols)

    if kind == "derived_call_site_noise":
        # D3 re-derived these cells from D2's committed raw fields because a
        # call-site-graded task must be denominated by fan_in, not by
        # true_reference_count. The derivation is reproduced, not cached.
        compare = _OPERATORS[selector["operator"]]
        floor = selector.get("requires_fan_in_at_least", 1)
        chosen = []
        for symbol in symbols:
            fan_in = symbol["fan_in"]
            if fan_in < floor:
                continue
            if compare(symbol["grep_hit_count"] / fan_in, selector["value"]):
                chosen.append(symbol["slug"])
        return sorted(chosen)

    raise PlanError("family %s uses unknown selector kind %r" % (family["id"], kind))


_TOKEN = re.compile(r"\{\{([a-z_]+)\}\}")


def _cell(symbol, family):
    prompt = family["arm_visible_prompt"]
    for token, value in (
        ("{{symbol_name}}", symbol["name"]),
        ("{{file_path}}", symbol["path"]),
        ("{{line}}", str(symbol["line"])),
        ("{{column}}", str(symbol["column"])),
        ("{{literal}}", symbol["name"]),
        ("{{receiver_type}}", symbol.get("receiver_type") or ""),
    ):
        if value:
            prompt = prompt.replace(token, value)
    left = _TOKEN.findall(prompt)
    if left:
        # An unfilled token would hand the arm a literally broken question and
        # grade the answer anyway. The cohort file not publishing the field the
        # prompt needs is a finding, not something to paper over with "".
        raise PlanError(
            "family %s cell %s: prompt still holds %s after substitution; the "
            "cohort file publishes no such field"
            % (family["id"], symbol["slug"], ", ".join(sorted(set(left))))
        )
    return {
        "slug": symbol["slug"],
        "name": symbol["name"],
        "path": symbol["path"],
        "line": symbol["line"],
        "column": symbol["column"],
        "prompt": prompt,
    }


def build_plan(registry, cohorts, max_cells_per_family=None):
    """Registry x cohort file -> the grid the sweep will execute."""
    index = _by_slug(cohorts)
    families = []
    voided = []
    reduction_families = {}

    for family in registry["families"]:
        if family.get("voided"):
            voided.append({"family": family["id"],
                           "reason": family.get("void_reason", "voided")})
            continue

        slugs = _select_slugs(family, cohorts)
        declared = family["cohort"]["n"]
        if len(slugs) != declared:
            raise PlanError(
                "family %s declares n=%d but its selector yields %d cells from "
                "the cohort file; the pre-registration and the run must agree"
                % (family["id"], declared, len(slugs))
            )

        dropped = []
        if max_cells_per_family is not None and len(slugs) > max_cells_per_family:
            dropped = slugs[max_cells_per_family:]
            slugs = slugs[:max_cells_per_family]
            reduction_families[family["id"]] = {
                "declared_n": declared,
                "ran_n": len(slugs),
                "dropped": dropped,
            }

        missing = [s for s in slugs if s not in index]
        if missing:
            raise PlanError(
                "family %s selects slugs absent from the cohort file's symbol "
                "table: %s" % (family["id"], ", ".join(missing))
            )

        families.append({
            "id": family["id"],
            "role": family["role"],
            "task_shape": family["task_shape"],
            "threshold": family.get("threshold"),
            "cells": [_cell(index[slug], family) for slug in slugs],
        })

    return {
        "schema_version": "discovery_sweep_plan_v1",
        "registry_id": registry.get("registry_id"),
        "corpus_commit": cohorts.get("corpus_commit"),
        "arms": ["treatment", "baseline"],
        "levels": ["tool", "agent"],
        "families": families,
        "voided": voided,
        "cohort_reduction": {
            "max_cells_per_family": max_cells_per_family,
            "families": reduction_families,
        },
    }
