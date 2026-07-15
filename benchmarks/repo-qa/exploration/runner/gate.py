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

# Argument keys through which a tool names a filesystem path. The value under
# any of these is containment-checked against the checkout root.
_PATH_ARG_KEYS = (
    "file_path",
    "path",
    "filename",
    "file",
    "dir",
    "directory",
    "root",
    "notebook_path",
)


def _path_arguments(arguments):
    for key in _PATH_ARG_KEYS:
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
        for key, value in _path_arguments(call.arguments):
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
