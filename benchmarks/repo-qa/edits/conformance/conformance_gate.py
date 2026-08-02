#!/usr/bin/env python3
"""Stage-3 mechanical convention-conformance GATE.

Where ``validate_edit_tasks.py`` (S3.1) proves a task BANK is well authored,
this module is the runtime GATE. It answers TWO questions, kept separate
because they are about different things and have different owners:

  * :func:`evaluate_agent_patch` (``conformance_agent_patch_v1``) -- did the
    AGENT's changed regions honour the task's house-style rule? THIS is the
    convention verdict the headline all-three-gates metric is asking for.
  * :func:`evaluate_task` (``conformance_gate_v1``) -- do the task's AUTHORED
    exemplar anchors still conform in the tree? This is BANK HEALTH: it
    measures our material, not the agent's work.

:func:`evaluate_run` (``conformance_gate_v2``) is what the runner calls, and
carries both. Scoring bank health alone was vacuous with respect to the agent:
exemplar anchors conform before the run and still conform after it no matter
what the agent wrote, so every patch passed convention for free.

ORACLE INDEPENDENCE (.claude/rules/bench-harness-oracle-independence.md)
-----------------------------------------------------------------------
The gate NEVER compiles or runs the task's authored ``convention.conforms_pattern``
regex -- on EITHER path. Its matching mechanism is a family of independent,
structural, bounded predicates (``_RULE_HANDLERS`` for anchors,
``_RULE_CANDIDATES`` for patched regions): each parses the bounded
evidence with its own logic (call-chain analysis, declaration-shape
parsing, throw/raise-construct detection) and decides conformance in Python
-- a different code object from the regex that authored the rule. Because
the two mechanisms are independent, a bug (or sabotage) in the authored
pattern cannot mask a wrong verdict here; the paired discrimination is
proven in ``tests/test_edit_conformance.py``
(``test_gate_ignores_sabotaged_conforms_pattern`` for anchors,
``test_agent_patch_ignores_the_sabotaged_conforms_pattern`` for patches).

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

On the agent-patch path the same discipline applies: an absent/empty patch
(PATCH_ABSENT) and a patch that touches nothing the rule governs
(PATCH_UNGOVERNED) both FAIL. An unproven convention claim is not a satisfied
one -- that free pass is exactly the hole this path exists to close.

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

OUTCOME_SCHEMA = "conformance_gate_v1"          # bank health (exemplar anchors)
AGENT_PATCH_SCHEMA = "conformance_agent_patch_v1"
RUN_OUTCOME_SCHEMA = "conformance_gate_v2"      # the runtime convention verdict


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
    # --- the agent-patch verdict (evaluate_agent_patch) ---
    PATCH_CONFORMS = "PATCH_CONFORMS"
    PATCH_NONCONFORMING = "PATCH_NONCONFORMING"
    PATCH_UNGOVERNED = "PATCH_UNGOVERNED"
    PATCH_ABSENT = "PATCH_ABSENT"


_PASS_REASONS = frozenset({Reason.CONFORMS, Reason.PATCH_CONFORMS})


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
# CANDIDATE detectors -- the agent-patch verdict's mechanism.
#
# A handler above answers "is there a CONFORMING construct here?", which is the
# right question for a bank exemplar (an anchor is authored to point at one).
# It is the WRONG question for an agent's patch: absence of a conforming
# construct cannot be told apart from "this region has nothing to do with the
# rule", so an agent that edits an unrelated file would score the same as one
# that broke the convention.
#
# A candidate detector answers the prior question -- "is there a construct this
# rule GOVERNS here, and does it conform?" -- returning
# ``[(label, conforms, start_offset, end_offset)]`` over a whole file's text.
# That separation is what makes the verdict discriminating:
#
#   no candidate touched -> not evidence either way          (region skipped)
#   any non-conforming   -> the patch breaks the convention  (FAIL)
#   all conforming       -> the patch honours it             (PASS)
#
# The offsets give each candidate a SPAN, and a candidate counts only when its
# span intersects a line the agent actually changed. Spans, not line proximity,
# are what make "touched" precise in both directions: an edit confined to a
# function's body still judges that function, while an untouched neighbour a few
# lines away is never charged to the agent.
#
# Like the handlers, these are the GATE's own structural mechanism and never
# compile the task's authored ``conforms_pattern`` (oracle independence).
# ---------------------------------------------------------------------------

_GO_FUNC = re.compile(
    r"^func\s+(?:\([^)]*\)\s*)?([A-Za-z0-9_]+)\s*\(([^)]*)\)\s*([^\{]*)\{",
    re.MULTILINE,
)
_TS_THROW = re.compile(r"\bthrow\s+([^\n;]+)")
_TS_TYPED_THROW = re.compile(r"^new\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def _mask_comments_and_strings(text):
    """Blank out //-comments and string-literal interiors, preserving offsets.

    Line-level and deliberately lightweight: a ``throw`` inside a comment or a
    string is prose, not a governed construct, and must not be charged to the
    agent. The mask replaces the masked characters with spaces so every
    surviving match's offsets still index into the ORIGINAL text.
    """
    out = list(text)
    in_string = None
    escaped = False
    i = 0
    n = len(text)
    while i < n:
        char = text[i]
        if in_string is not None:
            if char == "\n":
                in_string = None  # line-level: strings do not span lines here
            elif escaped:
                escaped = False
                out[i] = " "
            elif char == "\\":
                escaped = True
                out[i] = " "
            elif char == in_string:
                in_string = None
            else:
                out[i] = " "
            i += 1
            continue
        if char in ("'", '"', "`"):
            in_string = char
            i += 1
            continue
        if char == "/" and text[i : i + 2] == "//":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
            continue
        i += 1
    return "".join(out)


def _block_end(text, brace_index):
    """Offset just past the brace matching the one at ``brace_index``.

    Deliberately a plain brace counter, not a parser: it is used only to bound a
    construct's span for the touched test, and an unbalanced tail degrades to
    end-of-file (a WIDER span), so the failure mode is judging the agent's edit
    rather than silently exempting it.
    """
    depth = 0
    for i in range(brace_index, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
    return len(text)


def _throw_candidates(text):
    """Every ``throw`` STATEMENT is governed; only ``throw new <X>Error(``
    conforms. A bare ``throw 'string'`` is the violation this catches.

    Matching runs over a comment/string-masked copy of the text (offsets
    preserved), so ``// we throw here`` and ``"rethrow"`` are never governed;
    the evidence label is sliced from the ORIGINAL text.
    """
    masked = _mask_comments_and_strings(text)
    found = []
    for match in _TS_THROW.finditer(masked):
        thrown = text[match.start(1) : match.end(1)].strip()
        typed = _TS_TYPED_THROW.match(thrown)
        conforms = bool(typed) and typed.group(1).endswith("Error")
        found.append((f"throw {thrown}", conforms, match.start(), match.end()))
    return found


def _log_candidates(text):
    """Every diagnostic call is governed -- ``Log.*`` AND the raw ``console.*``
    the convention exists to displace. Only the shared namespace conforms.

    ``console`` is recognised anywhere before the trailing method
    (``window.console.error``, ``globalThis.console.warn``), not only as a
    two-part chain -- a longer chain was escaping governance entirely.
    """
    found = []
    for match in _CALL_CHAIN.finditer(text):
        parts = match.group(1).split(".")
        if len(parts) < 2:
            continue
        if parts[-2] == "console":
            conforms = False
        elif len(parts) == 2 and parts[0] == "Log":
            conforms = parts[1] in _LOG_METHODS
        else:
            continue
        found.append((match.group(1), conforms, match.start(), match.end()))
    return found


def _go_func_candidates(text, governs, conforms):
    """Shared walk over Go function declarations. ``governs``/``conforms`` decide
    membership and verdict per rule; the span is the whole function body."""
    found = []
    for match in _GO_FUNC.finditer(text):
        name, params, ret = match.groups()
        param_types = [p.strip() for p in params.split(",") if p.strip()]
        if not governs(name, param_types, ret):
            continue
        found.append((
            name, conforms(name, param_types, ret),
            match.start(), _block_end(text, match.end() - 1),
        ))
    return found


def _new_constructor_candidates(text):
    """Every ``func New*`` is governed; only a POINTER return conforms."""
    return _go_func_candidates(
        text,
        governs=lambda name, _p, _r: name.startswith("New"),
        conforms=lambda _n, _p, ret: "*" in ret,
    )


def _request_handler_candidates(text):
    """A request handler is governed when it is named ``handle*`` or already
    takes a ``*core.RequestEvent``; it conforms when it takes exactly that one
    parameter and returns ``error``. Naming the governed set explicitly is what
    keeps an ordinary helper function from being judged by this rule."""

    def _takes_event(params):
        return any(p.endswith("*core.RequestEvent") for p in params)

    return _go_func_candidates(
        text,
        governs=lambda name, params, _r: (
            _takes_event(params) or name.startswith("handle")
        ),
        conforms=lambda _n, params, ret: (
            len(params) == 1 and _takes_event(params) and ret.strip() == "error"
        ),
    )


# rule_id -> (governed file suffixes, candidate detector). A rule speaks only
# about the language it was written for: a changed region in a file the rule
# does not govern is not evidence, neither for nor against.
_RULE_CANDIDATES = {
    "ts-throw-typed-error": ((".ts", ".tsx", ".js", ".jsx"), _throw_candidates),
    "ts-central-log-namespace": ((".ts", ".tsx", ".js", ".jsx"), _log_candidates),
    "go-constructor-new-pointer": ((".go",), _new_constructor_candidates),
    "go-request-event-handler-shape": ((".go",), _request_handler_candidates),
}



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


# ---------------------------------------------------------------------------
# the AGENT-PATCH verdict
# ---------------------------------------------------------------------------


def _changed_line_spans(before, after):
    """Inclusive 1-based line spans of ``after`` that differ from ``before``.

    Pure text diff, no disk access. Splits on ``"\\n"`` ONLY -- the same
    coordinate system :func:`_line_span` counts in -- so ``\\f``/``\\v``/U+2028
    cannot skew a span. A PURE DELETION still records a span at the deletion
    point (its surviving neighbour lines): removing a governed construct's body
    must be judged, not silently exempted. Deterministic: difflib's opcodes come
    back in ascending order and are emitted in that order.
    """
    import difflib

    after_lines = after.split("\n")
    spans = []
    for tag, _i1, _i2, j1, j2 in difflib.SequenceMatcher(
        a=before.split("\n"), b=after_lines, autojunk=False
    ).get_opcodes():
        if tag == "equal":
            continue
        if j2 > j1:
            spans.append((j1 + 1, j2))
            continue
        # pure deletion (j1 == j2): the deletion point touches the adjacent
        # surviving line(s), so a mutated governed construct is still judged.
        start = max(j1, 1)
        end = min(j1 + 1, len(after_lines))
        if start <= end:
            spans.append((start, end))
    return spans


def _line_span(text, start_offset, end_offset):
    """The inclusive 1-based line span an offset range covers."""
    start_line = text.count("\n", 0, start_offset) + 1
    return start_line, start_line + text.count("\n", start_offset, end_offset)


def _intersects(span, spans):
    start, end = span
    return any(start <= other_end and other_start <= end
               for other_start, other_end in spans)


def _governed(rel, suffixes):
    return any(rel.endswith(suffix) for suffix in suffixes)


# Region rows carry the exact same shape as anchor rows; one constructor, two
# call-site names so each path still reads in its own vocabulary.
_region_outcome = _anchor_outcome


def _agent_patch_outcome(passed, reason, diagnostic, regions):
    return {
        "schema": AGENT_PATCH_SCHEMA,
        "passed": passed,
        "reason": reason,
        "diagnostic": diagnostic,
        "regions": regions,
    }


def _pristine_text(tree_dir, rel):
    abs_path = os.path.join(tree_dir, rel)
    if not os.path.isfile(abs_path):
        return ""
    with open(abs_path, "r", encoding="utf-8", errors="surrogateescape") as handle:
        return handle.read()


def evaluate_agent_patch(task, tree_dir, patch):
    """Judge the AGENT's changed regions against the task's convention rule.

    ``patch`` is the ``{relpath: new_text or None}`` map ``edit_patch`` captures
    by diffing the agent's checkout against the pristine tree. For every changed
    file the rule GOVERNS, the changed line spans are recovered against the
    pristine text, the rule's candidate detector is run over the new text, and a
    candidate is judged when its own span INTERSECTS a changed span:

      * no candidate touched -> not evidence either way, the file is skipped;
      * a touched construct that does not conform FAILS the patch;
      * at least one touched construct, all conforming, PASSES it.

    A patch that is absent/empty, or that touches nothing the rule governs,
    FAILS CLOSED (PATCH_ABSENT / PATCH_UNGOVERNED) -- an unproven convention
    claim is not a satisfied one. Deletions carry no evidence and are skipped.

    Deterministic: files are walked in sorted order and each file's candidates
    come back in ascending offset order.
    """
    rule_id = _rule_id_of(task)
    if rule_id is None or rule_id not in _RULE_CANDIDATES:
        return _agent_patch_outcome(
            False,
            Reason.RULE_MALFORMED if rule_id is None else Reason.RULE_UNSUPPORTED,
            f"{Reason.RULE_MALFORMED if rule_id is None else Reason.RULE_UNSUPPORTED}: "
            f"no candidate detector for rule {rule_id!r}",
            [],
        )
    suffixes, detect = _RULE_CANDIDATES[rule_id]

    if not patch:
        return _agent_patch_outcome(
            False, Reason.PATCH_ABSENT,
            f"{Reason.PATCH_ABSENT}: no agent patch to judge", [],
        )

    regions = []
    for rel in sorted(patch):
        content = patch[rel]
        # A deletion leaves no text to judge; the blast-radius gate owns whether
        # removing the file was allowed at all.
        if content is None or not _governed(rel, suffixes):
            continue
        try:
            changed = _changed_line_spans(_pristine_text(tree_dir, rel), content)
            if not changed:
                continue
            candidates = detect(content)
        except Exception as err:  # noqa: BLE001 -- NO EXCEPTION ESCAPES: a
            # pristine-read or diff fault fails closed exactly like a detector
            # fault, one red region row instead of an aborted verdict.
            regions.append(_region_outcome(
                rel, (0, 0), Reason.TOOL_FAILURE,
                detail=f"agent-patch evaluation raised: {err}",
            ))
            continue
        for label, conforms, start_offset, end_offset in candidates:
            span = _line_span(content, start_offset, end_offset)
            if not _intersects(span, changed):
                continue
            regions.append(_region_outcome(
                rel, span,
                Reason.PATCH_CONFORMS if conforms else Reason.PATCH_NONCONFORMING,
                evidence=label,
                detail=(
                    f"{label!r} honours the convention" if conforms
                    else f"{label!r} breaks the convention"
                ),
            ))

    if not regions:
        return _agent_patch_outcome(
            False, Reason.PATCH_UNGOVERNED,
            f"{Reason.PATCH_UNGOVERNED}: the patch changed no region governed by "
            f"rule {rule_id!r}; the convention claim is unproven",
            [],
        )
    failing = [r for r in regions if not r["passed"]]
    if failing:
        return _agent_patch_outcome(
            False, failing[0]["reason"],
            f"{failing[0]['reason']}: {len(failing)}/{len(regions)} changed "
            f"region(s) failed (first: {failing[0]['path']}:{failing[0]['lines']} "
            f"{failing[0]['evidence'] or failing[0]['detail']})",
            regions,
        )
    return _agent_patch_outcome(
        True, Reason.PATCH_CONFORMS,
        f"{Reason.PATCH_CONFORMS}: all {len(regions)} changed region(s) conform",
        regions,
    )


def evaluate_run(task, manifest, tree_dir, patch):
    """The runtime CONVENTION verdict for one edit attempt.

    Two independent components, neither merged into the other:

      * ``agent_patch`` -- did the AGENT's changed regions honour the rule? This
        is the question the headline metric is actually asking.
      * ``bank_health`` -- do the task's authored exemplar anchors still conform
        in the tree? This measures OUR material, not the agent's work.

    ``passed`` requires both. Bank health is not decoration: a convention
    verdict measured against defective exemplars is not a verdict, so it fails
    closed -- and because its reason codes stay ANCHOR_*/RULE_*, the scorer
    charges that failure to us rather than to the agent.
    """
    bank_health = evaluate_task(task, manifest, tree_dir)
    agent_patch = evaluate_agent_patch(task, tree_dir, patch)
    passed = agent_patch["passed"] and bank_health["passed"]
    if passed:
        diagnostic = agent_patch["diagnostic"]
    elif not agent_patch["passed"]:
        diagnostic = agent_patch["diagnostic"]
    else:
        diagnostic = f"bank health: {bank_health['diagnostic']}"
    rule_id = _rule_id_of(task)
    return {
        "schema": RUN_OUTCOME_SCHEMA,
        "task_id": task.get("id"),
        "rule_id": rule_id,
        "kind": _RULE_HANDLERS.get(rule_id, (None, None))[0] if rule_id else None,
        "passed": passed,
        "diagnostic": diagnostic,
        "agent_patch": agent_patch,
        "bank_health": bank_health,
    }


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
