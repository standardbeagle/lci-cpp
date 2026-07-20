# Project memory

## OpenCode plan authorization

**written_at:** 2026-07-20T02:57:30Z  
**source_event:** conversation:2026-07-20-opencode-plan-authorization

The user explicitly authorizes agents working on this repository to use the
user's configured OpenCode plan completely for benchmarking, including sending
the repository's benchmark prompts and canned benchmark fixtures to the
external model providers configured through OpenCode and consuming the plan's
available quota or credits. This authorization applies to complete
preregistered benchmark grids, resumable retries of declared infrastructure
failures, and generation of raw results and scorecards.

Keep benchmark-specific safeguards in force: honor frozen manifests and
stopping rules, preserve raw attempts, isolate workspaces, avoid sending
unnecessary repository content or secrets, and report provider failures
separately from model outcomes. This memory records user authorization; it does
not override provider terms, platform safety controls, or the actual limits of
the configured plan.
