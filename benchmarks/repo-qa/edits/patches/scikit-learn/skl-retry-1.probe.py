# Command skl-retry-1.probe is the discrimination probe for edit task
# skl-retry-1 (py-raise-valueerror-informative): linkage_tree in
# sklearn/cluster/_agglomerative.py must reject a bad `n_clusters` argument by
# raising ValueError with an explanatory message, not by asserting.
#
# The probe is a stdlib `ast` assertion over the materialized tree (cwd); it
# never imports sklearn and never matches source text with a regex.
# Exit 0: the function contains no `assert` on n_clusters and the guard raises
# ValueError with a non-empty message. Exit 1: otherwise.

import ast
import sys

TARGET_FILE = "sklearn/cluster/_agglomerative.py"
TARGET_FUNC = "linkage_tree"
ARG_NAME = "n_clusters"


def references_arg(node, name):
    return any(
        isinstance(child, ast.Name) and child.id == name for child in ast.walk(node)
    )


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
    for node in ast.walk(func):
        if isinstance(node, ast.Assert) and references_arg(node.test, ARG_NAME):
            print(
                f"probe: {TARGET_FUNC} in {TARGET_FILE} validates {ARG_NAME} "
                "with `assert` instead of raising an explanatory ValueError",
                file=sys.stderr,
            )
            sys.exit(1)
    for node in ast.walk(func):
        if isinstance(node, ast.If) and references_arg(node.test, ARG_NAME):
            for child in ast.walk(node):
                if isinstance(child, ast.Raise) and is_informative_valueerror(child):
                    sys.exit(0)
    print(
        f"probe: no informative ValueError guard on {ARG_NAME} found in "
        f"{TARGET_FUNC} ({TARGET_FILE})",
        file=sys.stderr,
    )
    sys.exit(1)


if __name__ == "__main__":
    main()
