"""Tool-isolation gate: enforcement, not declaration.

Exposing only the arm's tools to the provider is necessary but not sufficient
for a trustworthy benchmark, so the runner independently re-checks every tool
call the agent actually emitted against the effective allowlist. Two rejection
classes, both defended:

  * tool_not_allowed -- the call names a tool outside the arm's allowlist (a
    baseline run reaching for an LCI tool, or any arm reaching for Edit/Write/
    Bash, which are in no allowlist).
  * path_escape -- a path-bearing tool targets a location outside the clean
    checkout (an attempt to read the author-only task/annotation answer key, or
    to touch a file beyond the corpus).

A violation is a hard rejection: the runner records the run as a tool violation
and never scores it as an answer.
"""

import os

# Per-tool schemas keep path interpretation explicit. In particular, Glob's
# pattern is path-bearing while Grep's pattern is source text and is not.
_TOOL_ARGUMENTS = {
    "Read": {
        "required": {"file_path"},
        "allowed": {"file_path", "offset", "limit", "pages"},
        "paths": {"file_path"},
    },
    "Glob": {
        "required": {"pattern"},
        "allowed": {"pattern", "path"},
        "paths": {"pattern", "path"},
    },
    "Grep": {
        "required": {"pattern"},
        "allowed": {
            "pattern", "path", "glob", "output_mode", "-B", "-A", "-C",
            "context", "-n", "-i", "type", "head_limit", "offset", "multiline",
        },
        "paths": {"path"},
    },
    "mcp__lci__search": {
        "required": {"pattern"},
        "allowed": {"pattern", "max", "output", "path", "filter", "flags",
                    "include", "symbol_types", "patterns", "max_per_file",
                    "semantic", "languages"},
        "paths": {"path", "filter"},
    },
    "mcp__lci__get_context": {
        "required": set(),
        "allowed": {"id", "name", "file_id", "line", "column", "mode",
                    "include_full_symbol", "include_call_hierarchy",
                    "include_all_references", "include_dependencies",
                    "include_file_context", "include_quality_metrics", "max_depth",
                    "include_ai_text", "confidence_threshold", "exclude_test_files",
                    "include_sections", "exclude_sections", "symbol_id", "object_id",
                    "object_ids", "oid"},
        "paths": set(),
    },
    "mcp__lci__find_files": {
        "required": {"pattern"},
        "allowed": {"pattern", "max", "filter", "flags", "include_hidden",
                    "directory", "path"},
        "paths": {"pattern", "filter", "directory", "path"},
    },
    "mcp__lci__list_symbols": {
        "required": {"kind"},
        "allowed": {"kind", "file", "exported", "name", "receiver",
                    "min_complexity", "max_complexity", "min_params", "max_params",
                    "flags", "sort", "max", "offset", "include"},
        "paths": {"file"},
    },
    "mcp__lci__inspect_symbol": {
        "required": set(),
        "allowed": {"name", "id", "file", "type", "include", "max_depth"},
        "paths": {"file"},
    },
    "mcp__lci__browse_file": {
        "required": set(),
        "allowed": {"file", "path", "file_id", "kind", "exported", "sort", "max",
                    "include", "show_imports", "show_stats"},
        "paths": {"file", "path"},
    },
    # Kept explicit because these legacy names remain in the experiment's
    # allowlist even though the current server folds them into get_context.
    "mcp__lci__references": {
        "required": set(),
        "allowed": {"id", "name", "file", "path", "max", "include"},
        "paths": {"file", "path"},
    },
    "mcp__lci__callers": {
        "required": set(),
        "allowed": {"id", "name", "file", "path", "max", "include"},
        "paths": {"file", "path"},
    },
}

_INTEGER_ARGUMENTS = {
    "offset", "limit", "-B", "-A", "-C", "context", "head_limit", "max",
    "max_per_file", "file_id", "line", "column", "max_depth", "min_complexity",
    "max_complexity", "min_params", "max_params",
}
_BOOLEAN_ARGUMENTS = {
    "-n", "-i", "multiline", "semantic", "include_hidden", "exported",
    "include_full_symbol", "include_call_hierarchy", "include_all_references",
    "include_dependencies", "include_file_context", "include_quality_metrics",
    "include_ai_text", "exclude_test_files", "show_imports", "show_stats",
}
_NUMBER_ARGUMENTS = {"confidence_threshold"}
_ARRAY_ARGUMENTS = {"languages", "include_sections", "exclude_sections"}


def _valid_argument_type(key, value):
    if key in _INTEGER_ARGUMENTS:
        return isinstance(value, int) and not isinstance(value, bool)
    if key in _BOOLEAN_ARGUMENTS:
        return isinstance(value, bool)
    if key in _NUMBER_ARGUMENTS:
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if key in _ARRAY_ARGUMENTS:
        return isinstance(value, list) and all(isinstance(item, str) for item in value)
    return isinstance(value, str)


def _path_arguments(arguments, keys):
    for key in keys:
        value = arguments.get(key)
        if isinstance(value, str) and value:
            yield key, value


def _escapes(candidate, checkout_dir):
    """True when `candidate` resolves outside the checkout. Relative paths are
    joined onto the checkout, matching how a cwd-rooted agent resolves them."""
    root = os.path.realpath(checkout_dir)
    target = candidate if os.path.isabs(candidate) else os.path.join(root, candidate)
    resolved = os.path.realpath(target)
    return resolved != root and not resolved.startswith(root + os.sep)


def enforce(tool_calls, allowed_tools, checkout_dir):
    """Return an ordered list of violations (empty when the run is clean)."""
    allowed = set(allowed_tools)
    violations = []
    for index, call in enumerate(tool_calls):
        if call.name not in allowed:
            violations.append(
                {
                    "index": index,
                    "name": call.name,
                    "reason": "tool_not_allowed",
                    "detail": f"{call.name} is not in the {len(allowed)}-tool allowlist",
                }
            )
            continue
        if not isinstance(call.arguments, dict):
            violations.append({
                "index": index, "name": call.name,
                "reason": "malformed_arguments",
                "detail": "tool arguments must be an object",
            })
            continue
        schema = _TOOL_ARGUMENTS.get(call.name)
        if schema is None:
            violations.append({
                "index": index, "name": call.name,
                "reason": "unknown_tool_schema",
                "detail": "allowed tool has no local argument schema",
            })
            continue
        unknown = sorted(set(call.arguments) - schema["allowed"])
        if unknown:
            violations.append({
                "index": index, "name": call.name,
                "reason": "unknown_argument",
                "detail": f"unknown argument(s): {', '.join(unknown)}",
            })
            continue
        missing = sorted(schema["required"] - set(call.arguments))
        malformed = [
            key for key, value in call.arguments.items()
            if not _valid_argument_type(key, value)
            or (isinstance(value, str) and not value)
        ]
        if missing or malformed:
            bad = sorted(set(missing + malformed))
            violations.append({
                "index": index, "name": call.name,
                "reason": "malformed_arguments",
                "detail": f"missing or invalid argument(s): {', '.join(bad)}",
            })
            continue
        malformed_paths = sorted(
            key for key in schema["paths"]
            if key in call.arguments
            and (not isinstance(call.arguments[key], str) or not call.arguments[key])
        )
        if malformed_paths:
            violations.append({
                "index": index, "name": call.name,
                "reason": "malformed_arguments",
                "detail": (
                    "invalid path argument(s): " + ", ".join(malformed_paths)
                ),
            })
            continue
        for key, value in _path_arguments(call.arguments, schema["paths"]):
            if _escapes(value, checkout_dir):
                violations.append(
                    {
                        "index": index,
                        "name": call.name,
                        "reason": "path_escape",
                        "detail": f"{key}={value!r} resolves outside the checkout",
                    }
                )
    return violations
