"""D4 two-arm discovery sweep: cell planning, grading, and the sweep driver.

The modules here own the parts of the sweep that must be provable without a
model: which cells a family runs (`cells`), what the oracle's answer set is for
a task shape (`answer_sets`), how an arm's free-text answer is scored against
that set (`grading`), and the resume-safe driver that pairs both arms at both
measurement levels (`sweep`).

Arm execution itself is injected, so nothing here starts an agent or an MCP
server; `scripts/discovery_sweep.py` supplies the real executors.
"""
