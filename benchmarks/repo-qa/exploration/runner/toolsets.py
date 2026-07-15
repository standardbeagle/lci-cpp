"""Arm definitions: the two allowlists and the arm-specific tool instructions.

The experiment isolates ONE variable. Both arms get the same model, system
prompt, timeout, and clean checkout; they differ only in which tools are
exposed and a minimal instruction telling the agent how to navigate with them:

  * TREATMENT  -> LCI MCP semantic exploration tools + Read.
  * BASELINE   -> lexical Grep, file-discovery Glob, and Read only.

Neither arm is given Edit / Write (can't mutate the corpus) or Bash (can't
shell-escape to broader navigation), and the runner's gate re-enforces that.
The names are the exact identifiers the Claude CLI's --allowedTools expects, so
the recorded allowlist IS the effective one.
"""

from runner.adapter import AgentRequest

TREATMENT = "treatment"
BASELINE = "baseline"

# LCI MCP exploration surface (mcp__<server>__<tool>), server registered as "lci".
_LCI_TOOLS = (
    "mcp__lci__search",
    "mcp__lci__references",
    "mcp__lci__list_symbols",
    "mcp__lci__get_context",
    "mcp__lci__find_files",
    "mcp__lci__callers",
    "mcp__lci__inspect_symbol",
    "mcp__lci__browse_file",
)

# Sorted so the recorded allowlist is deterministic across runs/machines.
TREATMENT_TOOLS = tuple(sorted(_LCI_TOOLS + ("Read",)))
BASELINE_TOOLS = tuple(sorted(("Grep", "Glob", "Read")))

TREATMENT_INSTRUCTIONS = (
    "Explore with the LCI semantic tools: search for concepts, follow references "
    "and callers, and inspect symbols to locate the answer. Read files to confirm. "
    "Do not guess from memory; ground every claim in a location you found in this "
    "checkout."
)
BASELINE_INSTRUCTIONS = (
    "Explore with text search (Grep), file discovery (Glob), and file Read only. "
    "You have no semantic code-intelligence tools. Do not guess from memory; ground "
    "every claim in a location you found in this checkout."
)

_ARMS = {
    TREATMENT: (TREATMENT_TOOLS, TREATMENT_INSTRUCTIONS),
    BASELINE: (BASELINE_TOOLS, BASELINE_INSTRUCTIONS),
}


def arm_allowlist(arm):
    return _ARMS[arm][0]


def build_request(base, arm, checkout_dir, prompt):
    """Assemble the arm's request from the SHARED base config. Only the toolset
    and the tool instructions vary between arms; everything else is identical."""
    if arm not in _ARMS:
        raise ValueError(f"unknown arm {arm!r}; have {sorted(_ARMS)}")
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
