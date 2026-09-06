"""Selection-harness package for the E2.3 tool-selection A/B.

The epic measures which tool a model PICKS when offered LCI's toolset under a
given description variant. Everything here therefore reads one recorded
opencode event stream and answers a single question: what was the first
decisive tool call, and was it the right one.

Loaded by path under the name `selection_runner` (see scripts/selection_ab.py
and tests/test_selection_ab.py): benchmarks/repo-qa already has a top-level
`runner` package (exploration/runner), so putting toolcalling/ on sys.path
would make `import runner` order-dependent.
"""

from . import grade, matrix, trace, workspace  # noqa: F401

__all__ = ["grade", "matrix", "trace", "workspace"]
