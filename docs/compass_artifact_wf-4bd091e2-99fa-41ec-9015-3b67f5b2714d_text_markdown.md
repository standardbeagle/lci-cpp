# Claude Code Task Handoff Format Spec (2026)

For the LCI roadmap. Opinionated synthesis of Anthropic docs, GitHub Spec Kit, and current community practice. Skim this; apply it.

---

## TL;DR — repo layout

```
/CLAUDE.md                       # always-loaded project context (lean!)
/AGENTS.md                       # optional, mirrors CLAUDE.md for cross-tool agents
/ROADMAP_TASKS.md                # human-readable index of all tasks
/tasks/                          # one file per task, numbered
  0001-bootstrap-indexer.md
  0002-watcher-daemon.md
  ...
/.claude/
  commands/                      # (legacy) slash commands, still supported
  skills/                        # preferred: /skill-name with autoload
    run-task/SKILL.md            # e.g. `/run-task 0007`
    plan-task/SKILL.md           # e.g. `/plan-task 0007`
  agents/                        # custom sub-agents (.md w/ YAML frontmatter)
    implementer.md
    reviewer.md
  settings.json                  # hooks, permissions
/specs/                          # optional: deeper design docs referenced from tasks
  lci-architecture.md
```

Rationale: matches Anthropic's documented locations (`CLAUDE.md`, `.claude/commands`, `.claude/skills`, `.claude/agents`) and the dominant community pattern (numbered task files plus a top-level index, popularized by GitHub Spec Kit's `tasks.md` with `T001…`). The official docs explicitly say slash commands have been merged into Skills as of Claude Code v2.1.101 (April 2026), with `.claude/commands/` still supported but `.claude/skills/<name>/SKILL.md` recommended.

---

## 1. `CLAUDE.md` — keep it ~one screen

Anthropic's guidance and HumanLayer's analysis converge: **the system prompt already eats ~50 of the ~150–200 instructions a frontier model can reliably follow**. Bloat = ignored rules. Recommended sections:

```markdown
# LCI — Lightning Code Index

## Project Overview
One paragraph: what LCI is, who it's for, where the roadmap lives.

## Tech Stack
Languages, runtimes, key libraries (versions only where they matter).

## Repo Map
- `src/indexer/` — …
- `src/watcher/` — …
- `tasks/` — task specs Claude executes (see ROADMAP_TASKS.md)
- `specs/` — design docs referenced from tasks

## Commands
- Build: `…`
- Test (single file): `…`
- Lint/typecheck: `…`

## Workflow Rules
- Run typecheck + tests before declaring a task done.
- Prefer single-test runs over full suite.
- Never edit `tasks/*.md` while executing a task; only update status in `ROADMAP_TASKS.md`.

## Pointers (loaded on demand)
- Roadmap index: @ROADMAP_TASKS.md
- Architecture: @specs/lci-architecture.md
- Per-task spec: @tasks/<NNNN-slug>.md
```

Use `@path` imports rather than inlining content — Anthropic's docs and the HumanLayer "pointers over copies" rule both call this out. **Do not** put code-style guidance, lint rules, or per-task context here; defer to skills, hooks, and the task files. Tasks should *not* paste the CLAUDE.md content — Claude Code auto-loads it every session.

---

## 2. `ROADMAP_TASKS.md` — the index

Lightweight, scannable, machine-parseable. Borrows Spec Kit's `T001 [P] [US1]` convention (parallelizable / story label) but uses 4-digit IDs since you have a roadmap, not a single feature.

```markdown
# LCI Roadmap — Task Index

Status legend: `[ ]` todo · `[~]` in-progress · `[x]` done · `[!]` blocked
`[P]` = parallelizable with siblings in the same tier.

## Tier 1 — Indexer Core
- [ ] 0001 Bootstrap repo + CI                       → tasks/0001-bootstrap.md
- [ ] 0002 [P] Define index schema                   → tasks/0002-index-schema.md
- [ ] 0003 [P] Tokenizer + symbol extractor          → tasks/0003-tokenizer.md
- [ ] 0004 Persistence layer (depends: 0002)         → tasks/0004-persistence.md

## Tier 2 — Watcher
- [ ] 0010 File watcher daemon (depends: 0004)       → tasks/0010-watcher.md
- [ ] 0011 [P] Debounce + batching                   → tasks/0011-debounce.md

## Dependency graph
0001 → 0002, 0003
0002 → 0004
0004 → 0010
0010 → 0011
```

Justification: Spec Kit's `tasks-template.md` uses exactly this pattern (sequential IDs, `[P]` marker, dependency section). Claude Code's native Tasks API (v2.1.16+) supports `addBlockedBy` programmatically, but a markdown index is the durable, diff-able source of truth and is what the community converged on for hand-off-style workflows.

---

## 3. `tasks/NNNN-slug.md` — the task spec

YAML frontmatter for machine fields, narrative body for humans and Claude. Sections in this exact order — peripheral content (front + end) is what LLMs attend to best (HumanLayer, Claude Code system prompt analysis).

```markdown
---
id: 0010
title: File watcher daemon
tier: 2
status: todo                       # todo | in_progress | blocked | done
depends_on: [0004]
blocks: [0011, 0012]
parallelizable: false
agent: implementer                 # matches .claude/agents/implementer.md
model: sonnet                      # sonnet | opus | inherit
estimated_effort: M                # S | M | L  (≤2h / ≤1d / >1d)
owner: andy
created: 2026-05-05
---

# 0010 — File watcher daemon

## Context
2–4 sentences. Why this task exists, where it sits in the roadmap.
Reference deeper docs with @paths, do not inline them:
- Architecture: @specs/lci-architecture.md#watcher
- Upstream: @tasks/0004-persistence.md

## Goal
One sentence. The single observable outcome.

## Acceptance Criteria
Checkbox list. Each item must be independently verifiable.
- [ ] `lci watch <dir>` starts a daemon that survives SIGHUP.
- [ ] File create/modify/delete events are persisted within 200ms p95.
- [ ] `lci status` reports `watcher: running` and last-event timestamp.
- [ ] Integration test `tests/watcher/test_daemon.py` passes.
- [ ] Typecheck + lint clean.

## Implementation Notes
Bullet points, not prose. Constraints, gotchas, chosen approach.
- Use `notify` crate (already in Cargo.toml).
- No new top-level deps without updating CLAUDE.md.

## Files to Touch
Explicit allow-list. Anything outside requires a follow-up task.
- src/watcher/daemon.rs           (new)
- src/watcher/mod.rs              (modify)
- src/cli.rs                      (add `watch`, `status` subcommands)
- tests/watcher/test_daemon.py    (new)

## Test Plan
Exact commands Claude should run before marking done.
- `cargo test -p lci-watcher`
- `cargo clippy -- -D warnings`
- `python -m pytest tests/watcher/`

## Out of Scope
- Cross-platform polling fallback (→ task 0013)
- Remote/network watching (→ tier 4)

## References
- ADR-007 watcher choice: @specs/adr-007-watcher.md
- notify crate docs: https://docs.rs/notify
```

### Why these fields, briefly

- **`id`, `depends_on`, `blocks`, `parallelizable`** — direct port of Spec Kit's task model and the `addBlockedBy` semantics in Claude Code's native Tasks API (v2.1.16+).
- **`agent`** — names a sub-agent in `.claude/agents/`. Anthropic's sub-agent docs (code.claude.com/docs/en/sub-agents) require a `name`/`description`/`tools` frontmatter; a top-level `agent:` field on the task lets your `/run-task` skill dispatch via the Task tool.
- **`model`** — opt into Opus for plan-heavy tasks, Sonnet for execution. Mirrors `opusplan` mode and the per-subagent `model:` field documented by Anthropic.
- **`estimated_effort`** — community convention (S/M/L); helps you batch.
- **Acceptance Criteria as a checkbox list with concrete commands** — Spec Kit's "Independent Test" pattern plus Anthropic's "Claude performs better when it can check its own work" guidance. Given/When/Then is overkill for code tasks; checklists with runnable commands are what the spec-driven frameworks (Spec Kit, cc-sdd, BMAD) all converge on.
- **`Files to Touch` allow-list** — keeps Claude inside its lane and gives reviewers a diff expectation; pairs cleanly with a PreToolUse hook that blocks writes outside the list (Anthropic hooks reference).
- **`Test Plan`** with exact commands — this is the verification phase of Anthropic's documented "gather context → take action → verify results" loop.
- **`Out of Scope`** — Spec-driven frameworks (Anthropic SDD chapter, Addy Osmani's "good spec" post) all flag this as the highest-leverage section for preventing scope creep.

---

## 4. Plan-mode prompts: as **skills**, not inside task files

Put the dispatch logic in `.claude/skills/`, not in each task file. Two skills are enough:

```
.claude/skills/plan-task/SKILL.md     →  /plan-task 0010
.claude/skills/run-task/SKILL.md      →  /run-task 0010
```

`plan-task/SKILL.md`:
```markdown
---
name: plan-task
description: Read tasks/NNNN-*.md and produce an implementation plan in plan mode. Use before /run-task on any L-effort task.
allowed-tools: Read, Glob, Grep, WebFetch
argument-hint: <task-id>
---
Read tasks/$1-*.md. Read every file referenced via @paths.
Produce a numbered implementation plan covering:
1. Order of file edits.
2. Test strategy (which commands, in what order).
3. Risks and unknowns; ask clarifying questions if Acceptance Criteria are ambiguous.
Do NOT write code. Output only the plan.
```

`run-task/SKILL.md`:
```markdown
---
name: run-task
description: Execute a task spec end-to-end. Reads the task file, dispatches to the agent named in frontmatter, runs the Test Plan, and reports.
argument-hint: <task-id>
---
1. Read tasks/$1-*.md. Halt if status != todo.
2. Update ROADMAP_TASKS.md status to [~].
3. Delegate to the sub-agent named in `agent:` frontmatter via the Task tool, passing the full task body.
4. After the sub-agent returns: run every command in `## Test Plan`. If any fail, set status [!] and stop.
5. On success: tick all Acceptance Criteria boxes, set status [x], summarize the diff.
```

Justification: Anthropic's slash commands docs explicitly recommend Skills over `.claude/commands/` going forward. Putting the prompt in a skill (a) keeps task files declarative, (b) supports `$ARGUMENTS`, and (c) lets Claude auto-invoke when description matches. Armin Ronacher's plan-mode teardown confirms plan mode is "just a markdown file plus a state machine" — a skill replicates the value without forcing the Shift+Tab UX.

---

## 5. Sub-agent files (`.claude/agents/<name>.md`)

One per role, two minimum: `implementer` and `reviewer`. Anthropic's documented frontmatter:

```markdown
---
name: implementer
description: Use proactively to implement a task spec from tasks/. Writes code, runs the Test Plan, fixes failures.
tools: Read, Edit, Write, Bash, Glob, Grep
model: sonnet
permissionMode: default
color: blue
---
You are the LCI implementer. You receive a task spec body as input.
- Stay strictly inside the `## Files to Touch` allow-list.
- Run `## Test Plan` commands and iterate until they pass.
- If a command fails three times, return with status=blocked and a diagnosis.
- Never edit ROADMAP_TASKS.md; the orchestrator does that.
```

Frontmatter fields are from the official docs (code.claude.com/docs/en/sub-agents): `name`, `description`, `tools`, `model`, `permissionMode`, `color` (also `mcpServers`, `hooks`, `maxTurns`, `skills`, `effort`, `background`, `isolation` are supported per the same page). Action-oriented `description` ("Use proactively to…") is Anthropic's recommended phrasing for auto-delegation.

---

## 6. CLAUDE.md from inside tasks?

**No.** Don't reference CLAUDE.md from task files. Claude Code auto-loads it on every session and on every sub-agent launch (sub-agents inherit project CLAUDE.md per the docs). Re-referencing it wastes the peripheral-instruction budget that should be spent on task-specific content.

Do reference deeper specs (`@specs/lci-architecture.md`) — those are *not* auto-loaded.

---

## 7. 2026-specific things to actually use

- **Skills over commands.** `.claude/commands/` still works but Anthropic now recommends `.claude/skills/<name>/SKILL.md`. Skills support supporting files, autoload, and `disable-model-invocation: true` for things like `/deploy` you don't want Claude triggering on its own.
- **Native Tasks API (v2.1.16+).** Claude Code can track task state across sessions with `TaskCreate` / `TaskUpdate` / `addBlockedBy`. Optional, but if you set `CLAUDE_CODE_TASK_LIST_ID=lci-roadmap`, multi-session resumption works out of the box. Treat the markdown `ROADMAP_TASKS.md` as the durable spec; the Tasks API as ephemeral working state.
- **Opus plan mode.** `/model` → "Use Opus in plan mode, Sonnet 4.6 otherwise" is the current sweet spot for plan/execute split. Encode this in your `plan-task` skill via `model: opus`.
- **Hooks for guardrails.** A `PreToolUse` hook on `Edit|Write` that rejects paths outside the current task's `Files to Touch` is one of the highest-leverage things you can add — Anthropic's hooks docs explicitly call this pattern out, and HumanLayer/marmelab both recommend it. Hooks live in `.claude/settings.json`.
- **MCP only when justified.** The marmelab and Claude Directory writeups both warn against tool sprawl; each MCP server eats context. For LCI, Context7 (lib docs) and the GitHub MCP (PRs, issues) are the two with clear ROI.
- **AGENTS.md as a sibling.** If you want the same context to flow to Codex/Cursor/Copilot, mirror CLAUDE.md to `AGENTS.md` (open standard, supported across 11+ tools per the cross-platform analysis). Single source of truth: keep CLAUDE.md authoritative and `AGENTS.md` a one-line `@CLAUDE.md` import.

---

## Sources you'll want to keep handy

- Anthropic Claude Code best practices: `code.claude.com/docs/en/best-practices`
- Sub-agents reference: `code.claude.com/docs/en/sub-agents`
- Slash commands / Skills: `code.claude.com/docs/en/slash-commands`
- How Claude Code works (loop, plan mode tools): `code.claude.com/docs/en/how-claude-code-works`
- GitHub Spec Kit (T001 task format, parallel marker, dependency section): `github.com/github/spec-kit/blob/main/templates/tasks-template.md`
- HumanLayer "Writing a good CLAUDE.md" (instruction-budget analysis): `humanlayer.dev/blog/writing-a-good-claude-md`
- Armin Ronacher on what plan mode actually is: `lucumr.pocoo.org/2025/12/17/what-is-plan-mode/`
- cc-sdd / Kiro-style spec harness: `github.com/gotalab/cc-sdd`
- Addy Osmani on writing specs for agents: `addyosmani.com/blog/good-spec/`

That's the spec. Apply it; Claude Code generates the actual files.