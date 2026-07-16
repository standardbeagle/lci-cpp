"""Wire the sibling benchmark packages onto sys.path for the edit-mode runner.

The edit-mode runner REUSES the Stage-1 exploration runner (corpus
materialisation, the tool-isolation gate, the record log, the agent adapter),
the Stage-3 gates (``oracle_gate`` behaviour/blast-radius, ``conformance_gate``
convention), and the forge/validator helpers. Rather than reimplement any of
them, every edit module imports this first so the shared code resolves.

Importing this module is idempotent; the consumer (the unit tests and the
``exploration_runner.py`` CLI) only needs ``edits/runner`` itself on the path
to reach it.
"""

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))          # edits/runner
_EDITS = os.path.dirname(_HERE)                              # edits
_BENCH = os.path.dirname(_EDITS)                             # benchmarks/repo-qa

# _HERE first so sibling edit modules import as top-level names; the exploration
# runner is reached via its parent so ``import runner`` binds that package.
for _p in (
    _HERE,
    os.path.join(_BENCH, "exploration"),
    os.path.join(_BENCH, "scripts"),
    os.path.join(_EDITS, "oracles"),
    os.path.join(_EDITS, "conformance"),
):
    if _p not in sys.path:
        sys.path.insert(0, _p)
