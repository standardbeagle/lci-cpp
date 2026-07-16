#!/usr/bin/env python3
"""Stage-3 behaviour-oracle + blast-radius GATE.

Where ``conformance_gate.py`` (S3.2) judges whether an edit task's convention
exemplars mechanically conform, this module is the RUNTIME behaviour gate:
given one edit task and its reference (oracle) patch, it proves the edit
actually works and stays inside its declared blast radius. It emits a versioned
machine-readable outcome (``oracle_gate_v1``) carrying FOUR independently
versioned sub-outcomes:

  * ``existing_suite`` (``existing_suite_v1``) -- the repository's DECLARED
    existing test suite, run once on the patched tree as a regression guard.
  * ``discrimination`` (``discrimination_v1``) -- the task's behaviour command
    run on the PRISTINE tree (must be RED) and again on the ORACLE-PATCHED tree
    (must be GREEN). A test that passes on both, fails on both, or already
    passes pristine is REJECTED -- it discriminates nothing.
  * ``changed_path`` (``changed_path_v1``) -- every file the oracle patch touches
    must match a declared ``blast_radius.allow`` glob and respect ``max_files``.
  * ``api_impact`` (``api_impact_v1``) -- for each symbol the patch changes, LCI
    resolves references; any reference outside the declared allow scope is an
    escape (the public-API / call-hierarchy blast radius rippled past scope).

FAIL CLOSED (criterion 3)
-------------------------
A timeout, a missing command, a tool exception, an absent forged corpus, a
patch that escapes the blast radius, and a non-discriminating / wrong-direction
test all resolve to a deterministic FAIL with a stable reason code from
:class:`Reason`. Captured stdout/stderr is BOUNDED to ``_MAX_TAIL`` bytes. There
is no silent pass and no unbounded capture.

WORKTREE HYGIENE (criterion 4)
------------------------------
The pristine corpus tree is COPIED once into a throwaway temp worktree; every
run and the patch mutate only the copy. :func:`materialized_worktree` removes
the worktree on BOTH success and exception (``try/finally``), so the source
corpora are never mutated and no throwaway tree leaks.

HERMETIC + DETERMINISTIC (criterion 4)
--------------------------------------
No network, no clock, no randomness. Changed paths, impacted symbols and
escaped references are sorted before emit; :func:`to_json` renders a byte-stable
outcome.
"""

import contextlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

_SCRIPTS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "scripts",
)
if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)

# Reuse the manifest-translation approach from the S3.1 validator: liveness and
# the never-vendored forged-corpus location are decided the same way here, so
# the gate shares one oracle-independent source of truth with S3.1/S3.2.
import validate_edit_tasks as vedt  # noqa: E402
import validate_exploration_tasks as vet  # noqa: E402

AGGREGATE_SCHEMA = "oracle_gate_v1"
EXISTING_SUITE_SCHEMA = "existing_suite_v1"
DISCRIMINATION_SCHEMA = "discrimination_v1"
CHANGED_PATH_SCHEMA = "changed_path_v1"
API_IMPACT_SCHEMA = "api_impact_v1"

# Bound on captured stdout/stderr (criterion 3). Kept as a tail so the tail of a
# failing run's log survives while an unbounded flood cannot.
_MAX_TAIL = 4000

# Default per-command wall-clock budget. A command that outlives it fails closed
# with Reason.TIMEOUT rather than hanging the gate.
DEFAULT_TIMEOUT = 120


class Reason:
    """Stable reason-code enum. Codes are part of the machine contract and are
    never renamed once shipped. The four passing codes are SUITE_GREEN,
    DISCRIMINATES, WITHIN_SCOPE and NO_ESCAPE; everything else is fail-closed."""

    # existing suite
    SUITE_GREEN = "SUITE_GREEN"
    SUITE_RED = "SUITE_RED"
    # discrimination
    DISCRIMINATES = "DISCRIMINATES"
    NON_DISCRIMINATING = "NON_DISCRIMINATING"
    WRONG_DIRECTION = "WRONG_DIRECTION"
    RUN_ERROR = "RUN_ERROR"
    # changed path / blast radius
    WITHIN_SCOPE = "WITHIN_SCOPE"
    PATH_OUT_OF_SCOPE = "PATH_OUT_OF_SCOPE"
    TOO_MANY_FILES = "TOO_MANY_FILES"
    NO_CHANGES = "NO_CHANGES"
    BLAST_RADIUS_MALFORMED = "BLAST_RADIUS_MALFORMED"
    # api impact
    NO_ESCAPE = "NO_ESCAPE"
    API_IMPACT_ESCAPE = "API_IMPACT_ESCAPE"
    LCI_ABSENT = "LCI_ABSENT"
    # shared command-run failures
    TIMEOUT = "TIMEOUT"
    COMMAND_NOT_FOUND = "COMMAND_NOT_FOUND"
    COMMAND_ABSENT = "COMMAND_ABSENT"
    TOOL_FAILURE = "TOOL_FAILURE"
    # corpus location
    MANIFEST_ABSENT = "MANIFEST_ABSENT"


# ---------------------------------------------------------------------------
# glob matching for declared blast radius
#
# `*` matches within one path segment; `**` matches across segments (any depth).
# The translation is strict on purpose: a forbidden path must NOT sneak through
# a segment-scoped `*`, so blast-radius enforcement actually catches escapes.
# ---------------------------------------------------------------------------


def _glob_to_regex(glob):
    out = ["^"]
    i = 0
    n = len(glob)
    while i < n:
        char = glob[i]
        if char == "*":
            if glob[i : i + 2] == "**":
                if glob[i : i + 3] == "**/":
                    out.append("(?:.*/)?")
                    i += 3
                    continue
                out.append(".*")
                i += 2
                continue
            out.append("[^/]*")
            i += 1
            continue
        if char == "?":
            out.append("[^/]")
            i += 1
            continue
        out.append(re.escape(char))
        i += 1
    out.append("$")
    return re.compile("".join(out))


def _matches_any(path, globs):
    return any(_glob_to_regex(glob).match(path) is not None for glob in globs)


# ---------------------------------------------------------------------------
# changed-path (blast radius) enforcer
# ---------------------------------------------------------------------------


def _blast_radius_of(task):
    blast = task.get("blast_radius")
    if not isinstance(blast, dict):
        return None, None
    allow = blast.get("allow")
    if not isinstance(allow, list) or not allow:
        return None, None
    max_files = blast.get("max_files")
    if not isinstance(max_files, int):
        max_files = None
    return list(allow), max_files


def check_changed_paths(changed, blast_allow, max_files):
    """Enforce the declared changed-path scope over a patch's touched files.

    ``changed`` is the set of relative paths the oracle patch writes/removes.
    Fails closed on an empty change set, an out-of-scope path, or a max_files
    breach. Deterministic: changed and out_of_scope are emitted sorted.
    """
    changed = sorted(set(changed))
    if blast_allow is None:
        return _changed_path_outcome(
            False, Reason.BLAST_RADIUS_MALFORMED, changed, [], [], None,
            "blast_radius.allow is missing or not a non-empty list",
        )
    if not changed:
        return _changed_path_outcome(
            False, Reason.NO_CHANGES, changed, [], blast_allow, max_files,
            "oracle patch touches no files (nothing to gate)",
        )
    out_of_scope = sorted(p for p in changed if not _matches_any(p, blast_allow))
    if out_of_scope:
        return _changed_path_outcome(
            False, Reason.PATH_OUT_OF_SCOPE, changed, out_of_scope,
            blast_allow, max_files,
            f"{len(out_of_scope)} path(s) escape the declared blast radius "
            f"(first: {out_of_scope[0]})",
        )
    if max_files is not None and len(changed) > max_files:
        return _changed_path_outcome(
            False, Reason.TOO_MANY_FILES, changed, [], blast_allow, max_files,
            f"patch touches {len(changed)} file(s); max_files is {max_files}",
        )
    return _changed_path_outcome(
        True, Reason.WITHIN_SCOPE, changed, [], blast_allow, max_files,
        f"all {len(changed)} changed file(s) within the declared blast radius",
    )


def _changed_path_outcome(passed, reason, changed, out_of_scope, allow, max_files, detail):
    return {
        "schema": CHANGED_PATH_SCHEMA,
        "passed": passed,
        "reason": reason,
        "detail": detail,
        "changed": changed,
        "out_of_scope": out_of_scope,
        "allow": list(allow) if allow is not None else [],
        "max_files": max_files,
    }


def to_json(outcome):
    """Byte-stable rendering of any gate outcome (deterministic key order)."""
    return json.dumps(outcome, sort_keys=True, ensure_ascii=True, indent=2)
