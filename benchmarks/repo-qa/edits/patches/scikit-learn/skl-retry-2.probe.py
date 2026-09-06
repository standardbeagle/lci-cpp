# Command skl-retry-2.probe is the discrimination probe for edit task
# skl-retry-2 (py-raise-valueerror-informative): barycenter_weights in
# sklearn/manifold/_locally_linear.py must reject inconsistent X/indices
# arguments by raising ValueError with an explanatory message, not by
# asserting.
#
# The probe is a stdlib `ast` assertion over the materialized tree (cwd); it
# never imports sklearn and never matches source text with a regex.
# Exit 0: the function contains no `assert` statement and raises ValueError
# with a non-empty message on the inconsistent-shape branch. Exit 1:
# otherwise.

import ast
import sys

TARGET_FILE = "sklearn/manifold/_locally_linear.py"
TARGET_FUNC = "barycenter_weights"


def is_informative_valueerror(raise_node):
    exc = raise_node.exc
    if not (
        isinstance(exc, ast.Call)
        and isinstance(exc.func, ast.Name)
        and exc.func.id == "ValueError"
    ):
        return False
    return bool(exc.args)


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
    has_assert = False
    has_informative_raise = False
    for node in ast.walk(func):
        if isinstance(node, ast.Assert):
            has_assert = True
        elif isinstance(node, ast.Raise) and is_informative_valueerror(node):
            has_informative_raise = True
    if has_assert:
        print(
            f"probe: {TARGET_FUNC} in {TARGET_FILE} validates its arguments "
            "with `assert` instead of raising an explanatory ValueError",
            file=sys.stderr,
        )
        sys.exit(1)
    if has_informative_raise:
        sys.exit(0)
    print(
        f"probe: no informative ValueError guard found in {TARGET_FUNC} "
        f"({TARGET_FILE})",
        file=sys.stderr,
    )
    sys.exit(1)


if __name__ == "__main__":
    main()
