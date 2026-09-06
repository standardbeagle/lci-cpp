"""Render a structured LCI tool payload into the grader's `path:line` syntax.

The grader counts only cited `path:line` locations. The baseline arm's grep
output is rendered into that syntax by the executor, so the treatment arm's raw
JSON MUST be rendered too: handing the grader a JSON blob whose locations live
in separate `file_path` and `line` fields scores the treatment 0.0 by
construction on every family, and a zeroed row is indistinguishable from a real
loss downstream. Rendering one arm and not the other is a rigged instrument even
when the grader itself is correct.

Two rules keep this from becoming a silent adapter:

  * one renderer per tool RESPONSE SHAPE, keyed by tool name, and
  * an unrecognised shape -- an unknown tool, a payload that is not JSON, or a
    location that carries no path -- raises `UncitableToolResponse` with a
    reason. It never returns an empty answer, because an empty answer is graded
    as a wrong one.

`uncitable_location` is the reason reserved for the product gap where a
location arrives as `{FileID, Line, Column}` with no path (lci task
01M1VR4TBD7GXAXWBQXV2MCR49). The harness does not work around it; it names it,
so the sweep reports a broken cell rather than an attributable-looking zero.
"""

import json


class UncitableToolResponse(ValueError):
    """A tool payload cannot be rendered into `path:line` citations."""

    def __init__(self, reason, detail):
        super().__init__("%s: %s" % (reason, detail))
        self.reason = reason
        self.detail = detail


# Which LCI tool answers which task shape at the tool level, with the ARGUMENTS
# that tool actually takes. Probed against the live MCP surface (`tools/list`),
# not read off predictions.json's arms list: `callers` IS live and returns
# `file_path` + `call_lines`, i.e. real call sites; only `mcp__lci__references`
# is absent (tracked on D3 as 01M1VR4TAQ2W3N380QVG6WHQQQ). The treatment arm
# reaches every answer through the semantic surface; there is no grep fallback,
# by design.
def _anchor(cell, **extra):
    args = {"name": cell["name"], "line": cell["line"], "column": cell["column"]}
    args.update(extra)
    return args


_LCI_TOOL = {
    # Call sites and caller definitions both come from `callers`, which is the
    # only tool that emits caller locations with a path. get_context's call
    # hierarchy is bare NAMES (verified live), so it cannot answer either shape.
    "exhaustive_callers": ("callers", lambda c: {"name": c["name"], "max": 200}),
    "transitive_callers_depth2": ("callers", lambda c: {"name": c["name"], "max": 200}),
    "definition_lookup": ("list_symbols", lambda c: {"name": c["name"]}),
    # get_context emits no reference locations at all (the product gap above),
    # so the reference question goes through the search surface, which cites.
    "production_reference_partition": (
        "search",
        lambda c: {"pattern": c["name"], "max": 200},
    ),
    "implementations_lookup": (
        "get_context",
        lambda c: _anchor(c, include_sections=["structure"]),
    ),
    "literal_string_search": ("search", lambda c: {"pattern": c["name"], "max": 200}),
    "budget_constrained_variant": ("search", lambda c: {"pattern": c["name"], "max": 200}),
}


def tool_for(task_shape):
    """`(tool_name, build_args)` for a registered task shape, or `(None, None)`."""
    return _LCI_TOOL.get(task_shape, (None, None))


def _cite(path, line):
    return "%s:%d" % (path, int(line))


def _require_path(record, reason_detail):
    path = record.get("file_path") or record.get("file") or record.get("path")
    if not path:
        raise UncitableToolResponse("uncitable_location", reason_detail)
    return path


def _render_callers(payload, task_shape):
    """`callers`: call sites for the call-site shape, definitions otherwise."""
    lines = []
    want_call_sites = task_shape == "exhaustive_callers"
    for caller in payload.get("callers", []):
        path = _require_path(caller, "callers[] entry without a path: %r" % (caller,))
        if want_call_sites:
            for call_line in caller.get("call_lines") or [caller.get("line")]:
                lines.append(_cite(path, call_line))
        else:
            lines.append(_cite(path, caller["line"]))
    for site in payload.get("dynamic_call_sites", []):
        path = _require_path(site, "dynamic_call_sites entry without a path")
        lines.append(_cite(path, site["line"]))
    if not want_call_sites:
        for definition in payload.get("definitions", []):
            path = _require_path(definition, "definitions entry without a path")
            lines.append(_cite(path, definition["line"]))
    return lines


def _render_get_context(payload, task_shape):
    """`get_context`: the symbol locations in `contexts[]`.

    The call hierarchy and the caller list are bare names here, so nothing else
    in this payload is citable; a context that carries a line but no path is the
    FileID-only product gap and fails loud.
    """
    lines = []
    for context in payload.get("contexts", []):
        path = _require_path(
            context,
            "get_context location carries no path (file_id-only): %r"
            % ({k: context.get(k) for k in ("symbol_name", "file_id", "line")},),
        )
        lines.append(_cite(path, context["line"]))
    return lines


def _render_list_symbols(payload, task_shape):
    lines = []
    for symbol in payload.get("symbols", []):
        lines.append(_cite(_require_path(symbol, "symbols entry without a path"),
                           symbol["line"]))
    return lines


def _render_search(payload, task_shape):
    lines = []
    for result in payload.get("results", []):
        path = _require_path(result, "search result without a path")
        for hit in result.get("hits", []):
            lines.append(_cite(path, hit["line"]))
    return lines


_RENDERERS = {
    "callers": _render_callers,
    "get_context": _render_get_context,
    "list_symbols": _render_list_symbols,
    "search": _render_search,
}


def render_tool_answer(tool, task_shape, payload_text):
    """Render `payload_text` from `tool` into newline-separated `path:line`.

    Raises `UncitableToolResponse` rather than returning an unusable answer:
    an unknown tool shape, a payload that is not JSON, a payload whose top level
    is not an object, or a location with no path.
    """
    render = _RENDERERS.get(tool)
    if render is None:
        raise UncitableToolResponse(
            "unrecognised_tool_shape",
            "no citation renderer registered for tool %r; add one explicitly "
            "rather than grading its raw JSON" % (tool,),
        )
    try:
        payload = json.loads(payload_text)
    except (TypeError, ValueError) as exc:
        raise UncitableToolResponse(
            "unparseable_payload", "%s returned non-JSON text: %s" % (tool, exc))
    if not isinstance(payload, dict):
        raise UncitableToolResponse(
            "unparseable_payload",
            "%s returned a %s, not an object" % (tool, type(payload).__name__))
    return "\n".join(render(payload, task_shape))
