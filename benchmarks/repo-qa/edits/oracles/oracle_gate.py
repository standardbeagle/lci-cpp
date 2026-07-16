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


# ---------------------------------------------------------------------------
# bounded, fail-closed command runner
# ---------------------------------------------------------------------------


def _tail(text):
    """Keep the last _MAX_TAIL bytes so a flood cannot blow the outcome up."""
    if text is None:
        return ""
    if len(text) <= _MAX_TAIL:
        return text
    return text[-_MAX_TAIL:]


def _command_run(argv, exit_code, reason, stdout, stderr):
    if reason is not None:
        observed = "error"
    elif exit_code == 0:
        observed = "green"
    else:
        observed = "red"
    return {
        "argv": list(argv),
        "exit_code": exit_code,
        "reason": reason,
        "observed": observed,
        "stdout_tail": _tail(stdout),
        "stderr_tail": _tail(stderr),
    }


def run_command(argv, cwd, timeout=DEFAULT_TIMEOUT):
    """Run one argv, returning a bounded command-run dict. Fails closed.

    A missing binary -> COMMAND_NOT_FOUND, a timeout -> TIMEOUT, any other OS
    error -> TOOL_FAILURE. Captured stdout/stderr are always tail-bounded, so a
    runaway command cannot produce an unbounded outcome.
    """
    if not argv:
        return _command_run([], None, Reason.COMMAND_ABSENT, "", "no argv provided")
    try:
        proc = subprocess.run(
            list(argv),
            cwd=cwd,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except FileNotFoundError as err:
        return _command_run(argv, None, Reason.COMMAND_NOT_FOUND, "", str(err))
    except subprocess.TimeoutExpired as err:
        return _command_run(
            argv, None, Reason.TIMEOUT,
            err.stdout if isinstance(err.stdout, str) else "",
            f"timed out after {timeout}s",
        )
    except OSError as err:  # permission denied, exec format, etc.
        return _command_run(argv, None, Reason.TOOL_FAILURE, "", str(err))
    return _command_run(argv, proc.returncode, None, proc.stdout, proc.stderr)


# ---------------------------------------------------------------------------
# throwaway worktree + oracle-patch application (criterion 4)
# ---------------------------------------------------------------------------


@contextlib.contextmanager
def materialized_worktree(source_tree_dir, workspace_root=None):
    """Copy the pristine corpus tree into a throwaway worktree and yield it.

    The copy is removed on BOTH normal exit and exception, so the source corpora
    are never mutated and no throwaway tree leaks. Reads the source only.
    """
    holder = tempfile.mkdtemp(prefix="edit-oracle-", dir=workspace_root)
    dest = os.path.join(holder, "tree")
    try:
        shutil.copytree(source_tree_dir, dest)
        yield dest
    finally:
        shutil.rmtree(holder, ignore_errors=True)


def patch_changed_paths(oracle_patch):
    """The set of relative paths an oracle patch touches (no disk access)."""
    return sorted(oracle_patch)


def apply_patch(tree_dir, oracle_patch):
    """Apply an oracle patch (``{relpath: new_text_or_None}``) to a worktree.

    ``None`` deletes the file; a string (over)writes it. Refuses any path that
    escapes the worktree root, guaranteeing the source tree outside the copy is
    never written. Returns the sorted set of touched paths.
    """
    root = os.path.abspath(tree_dir)
    for rel, content in oracle_patch.items():
        dest = os.path.abspath(os.path.join(root, rel))
        if dest != root and not dest.startswith(root + os.sep):
            raise ValueError(f"oracle patch path escapes the worktree: {rel!r}")
        if content is None:
            if os.path.isfile(dest):
                os.remove(dest)
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "w", encoding="utf-8") as handle:
            handle.write(content)
    return patch_changed_paths(oracle_patch)


# ---------------------------------------------------------------------------
# red -> green discrimination classifier
# ---------------------------------------------------------------------------


def _discrimination_outcome(passed, reason, pristine, patched, detail):
    return {
        "schema": DISCRIMINATION_SCHEMA,
        "passed": passed,
        "reason": reason,
        "detail": detail,
        "pristine": pristine,
        "patched": patched,
    }


def discriminate(pristine, patched):
    """Classify a behaviour command's pristine vs oracle-patched runs.

    The only PASS is RED-pristine -> GREEN-patched (Reason.DISCRIMINATES). A run
    that failed closed (timeout/missing/tool error) makes the pair RUN_ERROR;
    passing both or failing both is NON_DISCRIMINATING; a pristine that already
    passes and then regresses is WRONG_DIRECTION. Every non-passing case is a
    hard reject -- a test that discriminates nothing must never be accepted.
    """
    for label, run in (("pristine", pristine), ("patched", patched)):
        if run["reason"] is not None:
            return _discrimination_outcome(
                False, Reason.RUN_ERROR, pristine, patched,
                f"{label} run failed closed: {run['reason']}",
            )
    pre_green = pristine["exit_code"] == 0
    post_green = patched["exit_code"] == 0
    if pre_green and post_green:
        return _discrimination_outcome(
            False, Reason.NON_DISCRIMINATING, pristine, patched,
            "behaviour passes on BOTH pristine and patched trees "
            "(discriminates nothing)",
        )
    if not pre_green and not post_green:
        return _discrimination_outcome(
            False, Reason.NON_DISCRIMINATING, pristine, patched,
            "behaviour fails on BOTH pristine and patched trees "
            "(patch does not make it pass)",
        )
    if pre_green and not post_green:
        return _discrimination_outcome(
            False, Reason.WRONG_DIRECTION, pristine, patched,
            "behaviour passes pristine and FAILS patched "
            "(wrong direction: the patch regresses it)",
        )
    return _discrimination_outcome(
        True, Reason.DISCRIMINATES, pristine, patched,
        "behaviour is RED pristine and GREEN patched (the edit discriminates)",
    )


# ---------------------------------------------------------------------------
# existing-suite regression guard (separate from discrimination)
# ---------------------------------------------------------------------------


def _existing_suite_outcome(passed, reason, run, detail):
    return {
        "schema": EXISTING_SUITE_SCHEMA,
        "passed": passed,
        "reason": reason,
        "detail": detail,
        "run": run,
    }


def run_existing_suite(command, cwd, timeout=DEFAULT_TIMEOUT):
    """Run the repository's DECLARED existing test suite on the patched tree.

    This is the regression guard and is kept SEPARATE from the task-specific
    discrimination test. Exit 0 -> SUITE_GREEN; a nonzero exit -> SUITE_RED; a
    timeout / missing command / tool error propagates its own fail-closed reason;
    a missing declared command is COMMAND_ABSENT (never a silent skip).
    """
    if not command:
        return _existing_suite_outcome(
            False, Reason.COMMAND_ABSENT, None,
            "no existing-suite command declared",
        )
    run = run_command(command, cwd, timeout)
    if run["reason"] is not None:
        return _existing_suite_outcome(
            False, run["reason"], run,
            f"existing suite failed closed: {run['reason']}",
        )
    if run["exit_code"] == 0:
        return _existing_suite_outcome(
            True, Reason.SUITE_GREEN, run,
            "existing suite is green on the patched tree",
        )
    return _existing_suite_outcome(
        False, Reason.SUITE_RED, run,
        f"existing suite is red on the patched tree (exit {run['exit_code']})",
    )


# ---------------------------------------------------------------------------
# public-API / call-hierarchy blast radius (LCI)
# ---------------------------------------------------------------------------


class LciRefsAdapter:
    """Resolve a symbol's references with the LCI binary.

    The gate depends only on the ``available()`` / ``refs()`` interface, so the
    hermetic tests inject a fake and the real corpus wiring supplies this. When
    the binary is absent the api-impact gate fails closed (LCI_ABSENT) rather
    than assuming no impact.
    """

    def __init__(self, lci_bin, timeout=DEFAULT_TIMEOUT):
        self.lci_bin = lci_bin
        self.timeout = timeout

    def available(self):
        if not self.lci_bin:
            return False
        return os.path.isfile(self.lci_bin) or shutil.which(self.lci_bin) is not None

    def refs(self, symbol, tree_dir):
        proc = subprocess.run(
            [self.lci_bin, "refs", symbol, "-r", tree_dir],
            capture_output=True,
            text=True,
            timeout=self.timeout,
        )
        if proc.returncode != 0:
            raise RuntimeError(
                f"lci refs {symbol!r} exited {proc.returncode}: "
                f"{_tail(proc.stderr)}"
            )
        paths = []
        for line in proc.stdout.splitlines():
            token = line.strip().split(":", 1)[0].strip()
            if token:
                paths.append(os.path.relpath(token, tree_dir) if os.path.isabs(token) else token)
        return paths


def _api_impact_outcome(passed, reason, symbols, escaped_refs, detail):
    return {
        "schema": API_IMPACT_SCHEMA,
        "passed": passed,
        "reason": reason,
        "detail": detail,
        "symbols": sorted(set(symbols)),
        "escaped_refs": escaped_refs,
    }


def check_api_impact(impacted_symbols, blast_allow, lci, tree_dir, timeout=DEFAULT_TIMEOUT):
    """Detect public-API / call-hierarchy breakage outside the declared scope.

    For each symbol the patch changes, LCI resolves references; any reference in
    a file that matches no ``blast_radius.allow`` glob is an ESCAPE -- the edit
    ripples beyond its declared blast radius. Fails closed when LCI is
    unavailable or errors. Deterministic: symbols and escaped refs are sorted.
    """
    symbols = sorted(set(impacted_symbols))
    if not symbols:
        return _api_impact_outcome(
            True, Reason.NO_ESCAPE, [], [],
            "no impacted symbols declared; no API blast radius to check",
        )
    if blast_allow is None:
        return _api_impact_outcome(
            False, Reason.BLAST_RADIUS_MALFORMED, symbols, [],
            "blast_radius.allow is missing; cannot bound API impact",
        )
    if lci is None or not lci.available():
        return _api_impact_outcome(
            False, Reason.LCI_ABSENT, symbols, [],
            "LCI unavailable; cannot verify public-API blast radius",
        )
    escaped = []
    for symbol in symbols:
        try:
            refs = lci.refs(symbol, tree_dir)
        except Exception as err:  # noqa: BLE001 -- fail closed on any tool fault
            return _api_impact_outcome(
                False, Reason.TOOL_FAILURE, symbols,
                sorted(escaped, key=lambda e: (e["symbol"], e["path"])),
                f"lci refs raised for {symbol!r}: {err}",
            )
        for path in refs:
            if not _matches_any(path, blast_allow):
                escaped.append({"symbol": symbol, "path": path})
    escaped = sorted(escaped, key=lambda e: (e["symbol"], e["path"]))
    if escaped:
        return _api_impact_outcome(
            False, Reason.API_IMPACT_ESCAPE, symbols, escaped,
            f"{len(escaped)} reference(s) escape the declared blast radius "
            f"(first: {escaped[0]['symbol']} @ {escaped[0]['path']})",
        )
    return _api_impact_outcome(
        True, Reason.NO_ESCAPE, symbols, [],
        f"all references to {len(symbols)} symbol(s) stay within the "
        f"declared blast radius",
    )


# ---------------------------------------------------------------------------
# aggregate gate
# ---------------------------------------------------------------------------


def _behavior_command_of(task):
    behavior = task.get("behavior")
    if not isinstance(behavior, dict):
        return []
    command = behavior.get("command")
    return list(command) if isinstance(command, list) else []


def _aggregate(task_id, existing_suite, discrimination, changed_path, api_impact):
    subgates = (
        ("discrimination", discrimination),
        ("changed_path", changed_path),
        ("api_impact", api_impact),
        ("existing_suite", existing_suite),
    )
    passed = all(sub["passed"] for _name, sub in subgates)
    if passed:
        diagnostic = "all four oracle sub-gates passed"
    else:
        first = next(sub for _name, sub in subgates if not sub["passed"])
        diagnostic = f"{first['reason']}: {first['detail']}"
    return {
        "schema": AGGREGATE_SCHEMA,
        "task_id": task_id,
        "passed": passed,
        "diagnostic": diagnostic,
        "existing_suite": existing_suite,
        "discrimination": discrimination,
        "changed_path": changed_path,
        "api_impact": api_impact,
    }


def _tool_failure_run(detail):
    return _command_run([], None, Reason.TOOL_FAILURE, "", detail)


def evaluate_oracle(
    task,
    source_tree_dir,
    oracle_patch,
    behavior_command=None,
    existing_suite_command=None,
    impacted_symbols=(),
    lci=None,
    timeout=DEFAULT_TIMEOUT,
    workspace_root=None,
):
    """Evaluate one edit task + its oracle patch against a pristine source tree.

    Materializes the tree into a throwaway worktree, runs the behaviour command
    RED (pristine) then GREEN (patched), runs the existing suite on the patched
    tree, enforces the changed-path scope, and checks the public-API blast
    radius. Returns an ``oracle_gate_v1`` aggregate. The worktree is cleaned on
    BOTH success and failure and the source tree is never mutated.
    """
    task_id = task.get("id")
    blast_allow, max_files = _blast_radius_of(task)

    changed_path = check_changed_paths(
        patch_changed_paths(oracle_patch), blast_allow, max_files
    )

    behavior_command = behavior_command or _behavior_command_of(task)

    try:
        with materialized_worktree(source_tree_dir, workspace_root) as tree:
            pristine = run_command(behavior_command or [], tree, timeout)
            apply_patch(tree, oracle_patch)
            patched = run_command(behavior_command or [], tree, timeout)
            discrimination = discriminate(pristine, patched)
            existing_suite = run_existing_suite(
                existing_suite_command, tree, timeout
            )
            api_impact = check_api_impact(
                impacted_symbols, blast_allow, lci, tree, timeout
            )
    except Exception as err:  # noqa: BLE001 -- fail closed on any orchestration fault
        run = _tool_failure_run(f"oracle orchestration raised: {err}")
        discrimination = _discrimination_outcome(
            False, Reason.TOOL_FAILURE, run, run,
            f"oracle orchestration raised: {err}",
        )
        existing_suite = _existing_suite_outcome(
            False, Reason.TOOL_FAILURE, run,
            f"oracle orchestration raised: {err}",
        )
        api_impact = _api_impact_outcome(
            False, Reason.TOOL_FAILURE, impacted_symbols, [],
            f"oracle orchestration raised: {err}",
        )

    return _aggregate(
        task_id, existing_suite, discrimination, changed_path, api_impact
    )


def _manifest_absent_aggregate(task, corpus_id, seed):
    blast_allow, max_files = _blast_radius_of(task)
    detail = (
        f"forged corpus for {corpus_id!r} seed {seed!r} not found "
        f"(never vendored)"
    )
    run = _command_run([], None, Reason.MANIFEST_ABSENT, "", detail)
    return _aggregate(
        task.get("id"),
        _existing_suite_outcome(False, Reason.MANIFEST_ABSENT, run, detail),
        _discrimination_outcome(
            False, Reason.MANIFEST_ABSENT, run, run, detail
        ),
        _changed_path_outcome(
            False, Reason.MANIFEST_ABSENT, [], [], blast_allow or [], max_files,
            detail,
        ),
        _api_impact_outcome(False, Reason.MANIFEST_ABSENT, [], [], detail),
    )


def evaluate_task_in_corpus(
    task, corpus_root, oracle_patch=None, **kwargs
):
    """Locate the forged manifest/tree for ``task`` and evaluate its oracle.

    Fails closed with MANIFEST_ABSENT when the never-vendored forged corpus is
    absent (the CI case) or when no oracle patch is supplied -- the gate is a
    judgement, not a linter. When both are present it delegates to
    :func:`evaluate_oracle` against the forged tree.
    """
    ref = task.get("manifest_ref", {})
    corpus_id = task.get("corpus")
    seed = ref.get("seed")
    manifest, tree_dir = vet.locate_manifest(corpus_root, corpus_id, seed)
    if manifest is None or oracle_patch is None:
        return _manifest_absent_aggregate(task, corpus_id, seed)
    return evaluate_oracle(task, tree_dir, oracle_patch, **kwargs)


def to_json(outcome):
    """Byte-stable rendering of any gate outcome (deterministic key order)."""
    return json.dumps(outcome, sort_keys=True, ensure_ascii=True, indent=2)
