# Command skl-module-1.probe is the discrimination probe for edit task
# skl-module-1 (py-all-export-list): sklearn/__check_build/__init__.py defines
# the public helper raise_build_error but leaves its public surface implicit;
# it must declare an explicit __all__ list naming its public names, the way
# its sibling package modules do.
#
# The probe is a stdlib `ast` assertion over the materialized tree (cwd); it
# never imports sklearn and never matches source text with a regex.
# Exit 0: a top-level __all__ assignment to a list of str names the module's
# public names. Exit 1: otherwise.

import ast
import sys

TARGET_FILE = "sklearn/__check_build/__init__.py"
EXPECTED_PUBLIC = {"raise_build_error"}


def all_export_names(tree):
    for stmt in tree.body:
        if not isinstance(stmt, ast.Assign):
            continue
        if not any(
            isinstance(target, ast.Name) and target.id == "__all__"
            for target in stmt.targets
        ):
            continue
        if not isinstance(stmt.value, ast.List):
            return None
        names = []
        for elt in stmt.value.elts:
            if not isinstance(elt, ast.Constant) or not isinstance(elt.value, str):
                return None
            names.append(elt.value)
        return names
    return None


def main():
    try:
        with open(TARGET_FILE, encoding="utf-8") as handle:
            tree = ast.parse(handle.read())
    except (OSError, SyntaxError) as exc:
        print(f"probe: cannot parse {TARGET_FILE}: {exc}", file=sys.stderr)
        sys.exit(1)
    names = all_export_names(tree)
    if names is not None and EXPECTED_PUBLIC <= set(names):
        sys.exit(0)
    print(
        f"probe: {TARGET_FILE} has no top-level __all__ list of str naming "
        f"its public names ({sorted(EXPECTED_PUBLIC)})",
        file=sys.stderr,
    )
    sys.exit(1)


if __name__ == "__main__":
    main()
