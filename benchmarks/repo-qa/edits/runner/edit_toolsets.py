"""Edit-mode arm definitions: the exploration arms PLUS the mutation tools.

Edit mode isolates the same single variable as exploration -- which navigation
surface the agent has (LCI semantic tools vs. lexical grep/glob) -- but the
agent must now PRODUCE a patch, so both arms additionally get ``Edit`` + ``Write``
(never ``Bash``). Everything else (model, timeout, checkout, the goal prompt) is
byte-identical across arms; only the toolset and the arm instruction differ, so
any capability gap is attributed to the navigation surface and nothing else.

The agent-visible prompt is the task's GOAL prompt ONLY (never the oracle key):
oracle isolation (criterion 3) is enforced by construction here -- the runner
never hands the exemplars, discrimination, rubric, or convention pattern to the
agent.
"""

import edit_paths  # noqa: F401  (sys.path bootstrap)

from runner import toolsets as nav
from runner.adapter import AgentRequest

TREATMENT = nav.TREATMENT
BASELINE = nav.BASELINE

# The mutation surface shared by both arms. Bash stays out (no shell escape);
# the runner's isolation gate re-checks every emitted call regardless.
_EDIT_TOOLS = ("Edit", "Write")

# Sorted so the recorded allowlist is deterministic across runs/machines.
TREATMENT_TOOLS = tuple(sorted(nav.TREATMENT_TOOLS + _EDIT_TOOLS))
BASELINE_TOOLS = tuple(sorted(nav.BASELINE_TOOLS + _EDIT_TOOLS))

_APPLY = (
    " Apply the change by editing files in THIS checkout with Edit/Write; keep "
    "the patch as small as possible and follow the conventions already present "
    "in the surrounding code."
)
TREATMENT_INSTRUCTIONS = nav.TREATMENT_INSTRUCTIONS + _APPLY
BASELINE_INSTRUCTIONS = nav.BASELINE_INSTRUCTIONS + _APPLY

# Generic edit-mode system prompt. Carries NO oracle material -- no target path,
# no symbol, no exemplar, no convention rule. It states only the shape of the job.
EDIT_SYSTEM_PROMPT = (
    "You are modifying an unfamiliar code checkout to satisfy a stated goal. "
    "Produce the smallest patch that achieves the goal while matching the house "
    "style already visible in this checkout. Ground every change in code you "
    "found here; do not rely on memory of any upstream project."
)

_ARMS = {
    TREATMENT: (TREATMENT_TOOLS, TREATMENT_INSTRUCTIONS),
    BASELINE: (BASELINE_TOOLS, BASELINE_INSTRUCTIONS),
}


def arm_allowlist(arm):
    return _ARMS[arm][0]


def build_request(base, arm, checkout_dir, prompt):
    """Assemble the arm's request from the SHARED base config. Only the toolset
    and the tool instruction vary; the prompt is the task's goal prompt ONLY."""
    if arm not in _ARMS:
        raise ValueError(f"unknown edit arm {arm!r}; have {sorted(_ARMS)}")
    allowed_tools, instructions = _ARMS[arm]
    return AgentRequest(
        model=base.model,
        system_prompt=base.system_prompt,
        timeout_seconds=base.timeout_seconds,
        checkout_dir=checkout_dir,
        allowed_tools=allowed_tools,
        tool_instructions=instructions,
        prompt=prompt,
    )
