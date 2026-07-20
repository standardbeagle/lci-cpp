# All-tools OpenCode replay

This preregistered pilot expands the recorded-request format replay to every live
LCI MCP tool. It contains 28 genuine MCP outputs: one success and one negative path
per tool. Two sanitized live OpenCode provider requests supply the model-specific
wire shape; each task derives a coherent user/tool-call/tool-result continuation.

The 448-cell plan is intentionally incapable of selecting a winner with only two
repetitions. Its purpose is to validate transport, task difficulty, negative-path
coverage, semantic heuristics, and exhaustive LLM follow-up before committing the
cost of a sufficiently repeated study.

Provider execution remains locked until `analysis_revision` names a committed
40-character revision and `provider_execution_allowed` is explicitly set true in a
later commit. The raw output matrix and dry preflight are frozen inputs.
