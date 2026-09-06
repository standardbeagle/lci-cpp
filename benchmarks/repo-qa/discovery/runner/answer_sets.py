"""Derive a family's oracle answer SET from a gopls answer key.

Each registered task shape grades a different projection of the same key, and
the projections are not interchangeable: `exhaustive_callers` grades CALL SITES
while `transitive_callers_depth2` grades caller DEFINITIONS. D3's review rewind
turned on exactly that distinction (call sites vs references), so the mapping
is explicit here and an unknown shape fails loud rather than defaulting to
whichever projection happens to be nearest.

An empty projection is returned as ungradable, not as an empty set that would
score every answer 0.0. `predictions.json` registry_findings.empty_answer_set_cell
binds this on D4: five cohort cells have fan_in = 0.

`literal_answer_set` is deliberately NOT a gopls query. The literal-string
control's oracle is a byte-exact scan implemented here, sharing no mechanism
with either arm.
"""

import os

from . import grading


class UnknownTaskShape(ValueError):
    """A family names a task shape this module cannot project a key onto."""


def _set(locations, note):
    unique = sorted({(loc[0], loc[1]) for loc in locations})
    if not unique:
        return {
            "locations": [],
            "gradable": False,
            "ungradable_reason": grading.REASON_EMPTY_ANSWER_SET,
            "note": note,
        }
    return {"locations": unique, "gradable": True, "ungradable_reason": None, "note": note}


def _call_sites(key):
    return [(c["call_site"]["path"], c["call_site"]["line"])
            for c in key["callers"] if c.get("depth", 1) == 1]


def _caller_definitions(key):
    return [(c["path"], c["line"]) for c in key["callers"] if c.get("depth", 1) <= 2]


def _definition(key):
    d = key["definition"]
    return [(d["path"], d["line"])]


def _production_references(key):
    return [(r["path"], r["line"]) for r in key["references"]
            if r.get("kind") == "production"]


def _implementations(key):
    return [(i["path"], i["line"]) for i in key["implementations"]]


_PROJECTIONS = {
    "exhaustive_callers": (
        _call_sites,
        "gopls call_hierarchy depth-1 callers, projected to their call sites",
    ),
    "transitive_callers_depth2": (
        _caller_definitions,
        "gopls call_hierarchy caller definitions to depth 2",
    ),
    "definition_lookup": (_definition, "gopls definition at the anchor"),
    "production_reference_partition": (
        _production_references,
        "gopls references, non-_test.go partition",
    ),
    "implementations_lookup": (_implementations, "gopls implementation at the anchor"),
}


def answer_set_for(task_shape, key):
    """Project a gopls key onto the answer set the task shape is graded against."""
    if task_shape == "budget_constrained_variant":
        # Graded on completion status, not on content: there is no answer set.
        return {
            "locations": [],
            "gradable": False,
            "ungradable_reason": grading.REASON_NOT_ANSWER_SET_GRADED,
            "note": "completion-status family; DNF rate is the metric",
        }
    if task_shape == "literal_string_search":
        raise UnknownTaskShape(
            "literal_string_search is graded by literal_answer_set(corpus_root, "
            "literal), not by a gopls key"
        )
    try:
        project, note = _PROJECTIONS[task_shape]
    except KeyError:
        raise UnknownTaskShape(
            "no answer-set projection registered for task shape %r; add one "
            "explicitly rather than grading against the nearest available "
            "projection" % (task_shape,)
        )
    return _set(project(key), note)


def literal_answer_set(corpus_root, literal, extensions=(".go",)):
    """Every `path:line` in the checkout whose text contains `literal`.

    The control family's oracle. Independent of gopls AND of both arms: a plain
    byte scan in Python, so a treatment win here cannot come from a shared
    matching mechanism.
    """
    hits = []
    for dirpath, dirnames, filenames in os.walk(corpus_root):
        dirnames[:] = sorted(d for d in dirnames if d != ".git")
        for name in sorted(filenames):
            if extensions and not name.endswith(tuple(extensions)):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, corpus_root).replace(os.sep, "/")
            try:
                with open(full, encoding="utf-8", errors="replace") as handle:
                    for lineno, line in enumerate(handle, start=1):
                        if literal in line:
                            hits.append((rel, lineno))
            except OSError as exc:
                raise RuntimeError("literal oracle cannot read %s: %s" % (full, exc))
    return _set(hits, "byte-exact substring scan over the checkout")
