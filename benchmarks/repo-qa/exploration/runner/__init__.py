"""Stage-1 exploration runner.

Runs each exploration task's prompt against a mutated corpus under two arms
(treatment = LCI semantic tools, baseline = lexical grep/glob/read only) with
an otherwise IDENTICAL agent configuration, so any capability gap is attributed
to the toolset and nothing else. The Claude invocation lives behind one
injectable adapter (`runner.adapter`): unit tests substitute a deterministic
fake agent; the guarded smoke command drives the real CLI.

Importing any submodule bootstraps the sibling `scripts/` dir onto sys.path so
the forge (materialisation + tree hashing) can be reused rather than
reimplemented.
"""

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_SCRIPTS = os.path.normpath(os.path.join(_HERE, "..", "..", "scripts"))
if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)
