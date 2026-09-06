# Command skl-log-1.probe is the discrimination probe for edit task skl-log-1
# (py-warnings-warn-channel): the non-fatal "cache loading failed" advisory in
# fetch_20newsgroups (sklearn/datasets/_twenty_newsgroups.py) must be routed
# through warnings.warn(...) with an explicit category, not printed ad hoc.
#
# The probe is a stdlib `ast` assertion over the materialized tree (cwd); it
# never imports sklearn and never matches source text with a regex.
# Exit 0: no print() call remains in the function and a warnings.warn call
# with a category is present. Exit 1: otherwise.

import ast
import sys

TARGET_FILE = "sklearn/datasets/_twenty_newsgroups.py"
TARGET_FUNC = "fetch_20newsgroups"


def is_print_call(call):
    return isinstance(call.func, ast.Name) and call.func.id == "print"


def is_warnings_warn(call):
    func = call.func
    return (
        isinstance(func, ast.Attribute)
        and func.attr == "warn"
        and isinstance(func.value, ast.Name)
        and func.value.id == "warnings"
    )


def has_category(call):
    return any(keyword.arg == "category" for keyword in call.keywords)


def main():
    try:
        with open(TARGET_FILE, encoding="utf-8") as handle:
            tree = ast.parse(handle.read())
    except (OSError, SyntaxError) as exc:
        print(f"probe: cannot parse {TARGET_FILE}: {exc}", file=sys.stderr)
        sys.exit(1)
    func = next(
        (
            node
            for node in tree.body
            if isinstance(node, ast.FunctionDef) and node.name == TARGET_FUNC
        ),
        None,
    )
    if func is None:
        print(f"probe: {TARGET_FUNC} not found in {TARGET_FILE}", file=sys.stderr)
        sys.exit(1)
    printed = False
    warned = False
    for node in ast.walk(func):
        if isinstance(node, ast.Call):
            if is_print_call(node):
                printed = True
            elif is_warnings_warn(node) and has_category(node):
                warned = True
    if printed:
        print(
            f"probe: {TARGET_FUNC} in {TARGET_FILE} emits its advisory via "
            "print() instead of warnings.warn(..., category=...)",
            file=sys.stderr,
        )
        sys.exit(1)
    if warned:
        sys.exit(0)
    print(
        f"probe: no warnings.warn(..., category=...) advisory found in "
        f"{TARGET_FUNC} ({TARGET_FILE})",
        file=sys.stderr,
    )
    sys.exit(1)


if __name__ == "__main__":
    main()
