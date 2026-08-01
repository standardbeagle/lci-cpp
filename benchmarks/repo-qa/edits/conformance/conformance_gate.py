#!/usr/bin/env python3
"""Stage-3 mechanical convention-conformance GATE.

Where ``validate_edit_tasks.py`` (S3.1) proves a task BANK is well authored,
this module is the runtime GATE: given one edit task, its forged
mutated-tree manifest, and the tree, it decides -- deterministically and
hermetically -- whether each convention exemplar anchor actually conforms to
the task's house-style rule, and emits a versioned machine-readable outcome
(``conformance_gate_v1``) carrying the rule id, the evaluated anchors, the
matched evidence, a pass/fail verdict, and a diagnostic.

ORACLE INDEPENDENCE (.claude/rules/bench-harness-oracle-independence.md)
-----------------------------------------------------------------------
The gate NEVER compiles or runs the task's authored ``convention.conforms_pattern``
regex. Its matching mechanism is a family of independent, structural,
bounded predicates (``_RULE_HANDLERS``): each parses the anchor's bounded
evidence with its own logic (call-chain analysis, declaration-shape
parsing, throw/raise-construct detection) and decides conformance in Python
-- a different code object from the regex that authored the rule. Because
the two mechanisms are independent, a bug (or sabotage) in the authored
pattern cannot mask a wrong verdict here; the paired discrimination is
proven in ``tests/test_edit_conformance.py``
(``test_gate_ignores_sabotaged_conforms_pattern``).

FAIL CLOSED (criterion 3)
-------------------------
Every abnormal input -- a MALFORMED anchor (absent/non-string path, absent or
non-integer line bounds, non-list identifiers, or an anchor that is not an
object at all), a missing anchor file, a decoy (dead/deprecated) twin, an
anchor outside the live path map, an out-of-range line bound, an absent target
identifier, an ambiguous (>1) structural match, a malformed rule, a rule kind
with no handler, or any tool exception -- resolves to a deterministic FAIL with
a stable reason code from :class:`Reason`. There is no silent pass, no
fallthrough, and NO EXCEPTION ESCAPES: every anchor field is shape-checked by
:func:`_anchor_shape_problem` before it is dereferenced, so a defective bank
entry scores one cell red instead of aborting the whole bank drain.

HERMETIC + DETERMINISTIC (criterion 4)
--------------------------------------
No network, no clock, no randomness. Anchors are evaluated and emitted in a
stable sort order; :func:`to_json` renders a byte-stable outcome.
"""

import os
import sys

_SCRIPTS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "scripts",
)
if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)

# Reuse the manifest-translation approach from the S3.1 validator: liveness and
# decoy membership are decided by SET MEMBERSHIP over the manifest, never by
# re-running the forge's mutation code (oracle independence, preserved here).
import validate_edit_tasks as vedt  # noqa: E402
import validate_exploration_tasks as vet  # noqa: E402  (bounded read helpers)

OUTCOME_SCHEMA = "conformance_gate_v1"


class Reason:
    """Stable reason-code enum. Codes are part of the machine contract and are
    never renamed once shipped."""

    CONFORMS = "CONFORMS"
    RULE_MALFORMED = "RULE_MALFORMED"
    RULE_UNSUPPORTED = "RULE_UNSUPPORTED"
    MANIFEST_ABSENT = "MANIFEST_ABSENT"
    ANCHOR_DECOY = "ANCHOR_DECOY"
    ANCHOR_NOT_LIVE = "ANCHOR_NOT_LIVE"
    ANCHOR_MALFORMED = "ANCHOR_MALFORMED"
    ANCHOR_MISSING = "ANCHOR_MISSING"
    ANCHOR_BOUND_STALE = "ANCHOR_BOUND_STALE"
    ANCHOR_IDENTIFIER_ABSENT = "ANCHOR_IDENTIFIER_ABSENT"
    MATCH_ABSENT = "MATCH_ABSENT"
    MATCH_AMBIGUOUS = "MATCH_AMBIGUOUS"
    TOOL_FAILURE = "TOOL_FAILURE"


_PASS_REASONS = frozenset({Reason.CONFORMS})


# ---------------------------------------------------------------------------
# Independent structural rule handlers.
#
# Each handler takes the anchor's bounded evidence string and returns the list
# of conforming constructs it found. 0 -> MATCH_ABSENT, exactly 1 -> conforms,
# >1 -> MATCH_AMBIGUOUS. NONE of these compile task['convention']['conforms_pattern'];
# each is its own mechanism, keyed to one convention family.
# ---------------------------------------------------------------------------

import re  # noqa: E402  (used only for the gate's OWN tokenizers, never the authored rule)

_CALL_CHAIN = re.compile(r"([A-Za-z_][A-Za-z0-9_.]*)\s*\(")
_LOG_METHODS = frozenset({"warn", "error", "info", "event", "warnOnce"})


def _throws_typed_error(segment):
    """retry-error-handling / ts-throw-typed-error.

    A conforming site THROWS a ``new`` construction of a class whose name ends
    in ``Error``. The Error-suffix decision is made in Python against the
    captured class name -- deliberately distinct from the authored regex, which
    bakes ``Error`` into the pattern itself.
    """
    found = []
    for match in re.finditer(
        r"throw\s+new\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(", segment
    ):
        class_name = match.group(1)
        if class_name.endswith("Error"):
            found.append(match.group(0))
    return found


def _log_namespace_calls(segment):
    """logging / ts-central-log-namespace.

    A conforming site routes through the shared ``Log`` namespace
    (``Log.warn`` / ``Log.error`` / ...). Raw ``console.*`` calls are NOT the
    convention and yield no match (so a console-only region fails MATCH_ABSENT).
    The call is recognised structurally by decomposing the callee chain, not by
    the authored alternation.
    """
    found = []
    for match in _CALL_CHAIN.finditer(segment):
        parts = match.group(1).split(".")
        if len(parts) == 2 and parts[0] == "Log" and parts[1] in _LOG_METHODS:
            found.append(match.group(1))
    return found


def _go_new_constructor(segment):
    """module-extraction-layout / go-constructor-new-pointer.

    A conforming site declares ``func New<Type>(...) *T`` -- a constructor
    RETURNING A POINTER. The pointer-return clause is checked in Python against
    the parsed return segment, not encoded in the authored regex.
    """
    found = []
    for match in re.finditer(
        r"^func\s+(New[A-Za-z0-9_]*)\s*\(([^)]*)\)\s*([^\{]*)\{",
        segment,
        re.MULTILINE,
    ):
        name, _params, ret = match.groups()
        if "*" in ret:
            found.append(name)
    return found


def _go_request_event_handler(segment):
    """api-shape-consistency / go-request-event-handler-shape.

    A conforming handler takes exactly one ``*core.RequestEvent`` parameter and
    returns ``error``. Parameter count, parameter type, and return type are all
    decided in Python against the parsed signature.
    """
    found = []
    for match in re.finditer(
        r"^func\s+(?:\([^)]*\)\s*)?([A-Za-z0-9_]+)\s*\(([^)]*)\)\s*"
        r"([A-Za-z0-9_.\*\[\]]*)\s*\{",
        segment,
        re.MULTILINE,
    ):
        name, params, ret = match.groups()
        param_types = [p.strip() for p in params.split(",") if p.strip()]
        if len(param_types) != 1:
            continue
        if not param_types[0].endswith("*core.RequestEvent"):
            continue
        if ret.strip() != "error":
            continue
        found.append(name)
    return found


# rule_id -> (kind label, independent predicate). One representative rule per
# convention family is supported; every other rule_id fails closed as
# RULE_UNSUPPORTED rather than being silently accepted.
_RULE_HANDLERS = {
    "ts-throw-typed-error": ("throws-typed-error", _throws_typed_error),
    "ts-central-log-namespace": ("log-namespace-call", _log_namespace_calls),
    "go-constructor-new-pointer": ("constructor-new-pointer", _go_new_constructor),
    "go-request-event-handler-shape": (
        "request-event-handler-shape",
        _go_request_event_handler,
    ),
}


def supported_rule_ids():
    return sorted(_RULE_HANDLERS)


# ---------------------------------------------------------------------------
# anchor evaluation
# ---------------------------------------------------------------------------


def _anchor_outcome(path, lines, reason, evidence="", detail=""):
    return {
        "path": path,
        "lines": list(lines),
        "passed": reason in _PASS_REASONS,
        "reason": reason,
        "evidence": evidence,
        "detail": detail,
    }


def _anchor_shape_problem(anchor):
    """Validate an anchor's SHAPE before any field is dereferenced.

    Returns ``(path, lines, problem)``. ``problem`` is None when the anchor is
    well formed; otherwise it describes the defect and ``path``/``lines`` are
    the best-effort renderable values for the outcome. Nothing downstream may
    index into ``lines`` or subscript ``anchor`` until this returns None --
    that unguarded access was the fail-open hole this closes.
    """
    if not isinstance(anchor, dict):
        return "", [], f"anchor is {type(anchor).__name__}, not an object"

    path = anchor.get("path")
    lines = anchor.get("lines")
    safe_lines = list(lines) if isinstance(lines, list) else []
    safe_path = path if isinstance(path, str) else ""

    if not isinstance(path, str) or not path:
        return safe_path, safe_lines, "anchor.path is missing or not a string"
    if not isinstance(lines, list) or not lines:
        return safe_path, safe_lines, "anchor.lines is missing or not a non-empty list"
    if not all(isinstance(n, int) and not isinstance(n, bool) for n in lines):
        return safe_path, safe_lines, f"anchor.lines {lines!r} holds a non-integer bound"

    identifiers = anchor.get("target_identifiers", [])
    if not isinstance(identifiers, list):
        return safe_path, safe_lines, "anchor.target_identifiers is not a list"
    if not all(isinstance(i, str) for i in identifiers):
        return safe_path, safe_lines, "anchor.target_identifiers holds a non-string"
    return safe_path, safe_lines, None


def _anchor_sort_key(anchor):
    """Total, malformed-tolerant sort key. Malformed anchors sort last (their
    lines degrade to ()), so their presence cannot reorder well-formed ones."""
    path, lines, problem = _anchor_shape_problem(anchor)
    return (problem is not None, path, tuple(lines))


def _evaluate_anchor(anchor, handler, manifest, tree_dir):
    """Evaluate one anchor. Fails closed on every abnormal condition."""
    path, lines, problem = _anchor_shape_problem(anchor)
    if problem is not None:
        return _anchor_outcome(
            path, lines, Reason.ANCHOR_MALFORMED, detail=problem,
        )

    if path in vedt.decoy_paths(manifest):
        return _anchor_outcome(
            path, lines, Reason.ANCHOR_DECOY,
            detail="anchor path is a dead/deprecated decoy twin",
        )
    if path not in vedt.live_paths(manifest):
        return _anchor_outcome(
            path, lines, Reason.ANCHOR_NOT_LIVE,
            detail="anchor path is not a live mutated file of the corpus",
        )

    abs_path = os.path.join(tree_dir, path)
    if not os.path.isfile(abs_path):
        return _anchor_outcome(
            path, lines, Reason.ANCHOR_MISSING,
            detail="anchor file does not exist in the forged tree",
        )

    start = lines[0]
    end = lines[-1]
    try:
        total = vet._line_count(abs_path)
    except OSError as err:
        return _anchor_outcome(
            path, lines, Reason.TOOL_FAILURE,
            detail=f"could not read anchor file: {err}",
        )
    if end < start or start < 1 or end > total:
        return _anchor_outcome(
            path, lines, Reason.ANCHOR_BOUND_STALE,
            detail=f"line bound {lines} does not fit file length {total}",
        )

    try:
        segment = vet._read_range(abs_path, start, end)
    except OSError as err:
        return _anchor_outcome(
            path, lines, Reason.TOOL_FAILURE,
            detail=f"could not read anchor evidence: {err}",
        )

    for identifier in anchor.get("target_identifiers", []):
        if not vet._word_present(identifier, segment):
            return _anchor_outcome(
                path, lines, Reason.ANCHOR_IDENTIFIER_ABSENT,
                detail=f"target identifier {identifier!r} absent from evidence",
            )

    try:
        matches = handler(segment)
    except Exception as err:  # noqa: BLE001 -- fail closed on ANY handler fault
        return _anchor_outcome(
            path, lines, Reason.TOOL_FAILURE,
            detail=f"rule handler raised: {err}",
        )

    if not matches:
        return _anchor_outcome(
            path, lines, Reason.MATCH_ABSENT,
            detail="no conforming construct found in bounded evidence",
        )
    if len(matches) > 1:
        return _anchor_outcome(
            path, lines, Reason.MATCH_AMBIGUOUS,
            evidence=str(matches[0]),
            detail=f"{len(matches)} conforming constructs; expected exactly one",
        )
    return _anchor_outcome(
        path, lines, Reason.CONFORMS, evidence=str(matches[0]),
        detail="bounded evidence conforms to the convention",
    )


# ---------------------------------------------------------------------------
# task-level gate
# ---------------------------------------------------------------------------


def _rule_id_of(task):
    convention = task.get("convention")
    if not isinstance(convention, dict):
        return None
    rule_id = convention.get("rule_id")
    return rule_id if isinstance(rule_id, str) and rule_id else None


def _gate_outcome(task_id, rule_id, kind, passed, diagnostic, anchors):
    return {
        "schema": OUTCOME_SCHEMA,
        "task_id": task_id,
        "rule_id": rule_id,
        "kind": kind,
        "passed": passed,
        "diagnostic": diagnostic,
        "anchors": anchors,
    }


def evaluate_task(task, manifest, tree_dir):
    """Evaluate one edit task's convention exemplars against a forged tree.

    Returns a ``conformance_gate_v1`` outcome dict. Deterministic: anchors are
    evaluated and emitted sorted by ``(path, lines)``.
    """
    task_id = task.get("id")
    rule_id = _rule_id_of(task)

    if rule_id is None:
        return _gate_outcome(
            task_id, None, None, False,
            f"{Reason.RULE_MALFORMED}: convention.rule_id missing or empty", [],
        )
    if rule_id not in _RULE_HANDLERS:
        return _gate_outcome(
            task_id, rule_id, None, False,
            f"{Reason.RULE_UNSUPPORTED}: no gate handler for rule {rule_id!r}", [],
        )

    kind, handler = _RULE_HANDLERS[rule_id]

    exemplars = task.get("exemplars")
    if not isinstance(exemplars, list) or not exemplars:
        return _gate_outcome(
            task_id, rule_id, kind, False,
            f"{Reason.RULE_MALFORMED}: task has no exemplar anchors", [],
        )

    anchors = [
        _evaluate_anchor(anchor, handler, manifest, tree_dir)
        for anchor in sorted(exemplars, key=_anchor_sort_key)
    ]

    passed = all(anchor["passed"] for anchor in anchors)
    if passed:
        diagnostic = f"{Reason.CONFORMS}: all {len(anchors)} anchor(s) conform"
    else:
        failing = [a for a in anchors if not a["passed"]]
        diagnostic = (
            f"{failing[0]['reason']}: {len(failing)}/{len(anchors)} anchor(s) "
            f"failed (first: {failing[0]['path']}:{failing[0]['lines']})"
        )
    return _gate_outcome(task_id, rule_id, kind, passed, diagnostic, anchors)


def evaluate_task_in_corpus(task, corpus_root):
    """Locate the forged manifest/tree for ``task`` and evaluate it.

    Fails closed with MANIFEST_ABSENT when the (never-vendored) forged corpus is
    not present, rather than skipping -- the gate is a judgement, not a linter.
    """
    ref = task.get("manifest_ref", {})
    manifest, tree_dir = vet.locate_manifest(
        corpus_root, task.get("corpus"), ref.get("seed")
    )
    rule_id = _rule_id_of(task)
    kind = _RULE_HANDLERS.get(rule_id, (None, None))[0] if rule_id else None
    if manifest is None:
        return _gate_outcome(
            task.get("id"), rule_id, kind, False,
            f"{Reason.MANIFEST_ABSENT}: forged corpus for "
            f"{task.get('corpus')!r} seed {ref.get('seed')!r} not found", [],
        )
    return evaluate_task(task, manifest, tree_dir)


def to_json(outcome):
    """Byte-stable rendering of a gate outcome (deterministic key order)."""
    import json

    return json.dumps(outcome, sort_keys=True, ensure_ascii=True, indent=2)
