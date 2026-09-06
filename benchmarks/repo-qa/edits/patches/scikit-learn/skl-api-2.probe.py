# Command skl-api-2.probe is the discrimination probe for edit task skl-api-2
# (py-fit-returns-self): BernoulliRBM.partial_fit in
# sklearn/neural_network/_rbm.py must honour the chainable-return contract its
# docstring declares ("Returns: self") - the method's last statement must be
# `return self`, never a bare `self._fit(...)` call whose result is discarded.
#
# The probe is a stdlib `ast` assertion over the materialized tree (cwd); it
# never imports sklearn and never matches source text with a regex.
# Exit 0: the method ends in `return self`. Exit 1: otherwise.

import ast
import sys

TARGET_FILE = "sklearn/neural_network/_rbm.py"
TARGET_CLASS = "BernoulliRBM"
TARGET_METHOD = "partial_fit"


def find_method(tree, class_name, method_name):
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and node.name == class_name:
            for item in node.body:
                if isinstance(item, ast.FunctionDef) and item.name == method_name:
                    return item
    return None


def ends_with_return_self(func):
    last = func.body[-1]
    return (
        isinstance(last, ast.Return)
        and isinstance(last.value, ast.Name)
        and last.value.id == "self"
    )


def main():
    try:
        with open(TARGET_FILE, encoding="utf-8") as handle:
            tree = ast.parse(handle.read())
    except (OSError, SyntaxError) as exc:
        print(f"probe: cannot parse {TARGET_FILE}: {exc}", file=sys.stderr)
        sys.exit(1)
    method = find_method(tree, TARGET_CLASS, TARGET_METHOD)
    if method is None:
        print(
            f"probe: {TARGET_CLASS}.{TARGET_METHOD} not found in {TARGET_FILE}",
            file=sys.stderr,
        )
        sys.exit(1)
    if ends_with_return_self(method):
        sys.exit(0)
    print(
        f"probe: {TARGET_CLASS}.{TARGET_METHOD} in {TARGET_FILE} ends with "
        f"a {type(method.body[-1]).__name__}, not `return self` - the "
        "chainable-return contract is broken",
        file=sys.stderr,
    )
    sys.exit(1)


if __name__ == "__main__":
    main()
