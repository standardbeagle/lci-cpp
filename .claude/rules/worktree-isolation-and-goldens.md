# Worktree Isolation, Scope Gates, and Golden Portability

Lessons from draining a task while a sibling live session held uncommitted
out-of-scope changes in the primary checkout (task `01KXECD66GVAX33XXGY5F99FQ4`,
"FIX: pipeline_scanner detect_language drift", commits `9f380a8` RED / `f941b0e`
GREEN, 2026-07-14).

## 1. Drain a task under a dirtied shared tree via a clean worktree + explicit-diff scope-check

Symptom: `file_scope` step fails for ANY task, even one with a narrow,
correct `fileScope`, because the server's scope-check git-diffs the *whole*
primary working tree — and a second live loop session (different
`leaseHolder`) had uncommitted, out-of-scope changes sitting there
(`benchmarks/repo-qa/exploration/corpora.json`,
`benchmarks/repo-qa/scripts/exploration_corpus_forge.py`).

Recipe:
1. `git worktree add ../lci-cpp-langmap -b worktrack/langmap-fix main` — a
   clean worktree off `main`, isolated from the dirtied primary checkout.
2. Implement + commit the RED/GREEN pair *there*.
3. Re-run the `file_scope` step via `task_workflow_step_evaluate` passing an
   **explicit committed-diff file list** (the two touched files) instead of
   letting the server git-diff the (still-dirty) primary tree. This is what
   flipped scope-check from `failed/withinScope:false` (attempt 1, saw the
   sibling session's paths) to `passed` (attempt 2, saw only the task's own
   files).
4. Run every subsequent build/ctest command step with `cwd=<worktree>`, not
   the primary checkout.
5. Record the takeover/isolation reason as a task comment (who else held the
   lease, what evidence, which paths) so a resuming session doesn't have to
   re-derive it — see `reference-port-discipline.md` rule 9 for the sibling
   lease-evidence discipline this pairs with.

This is the reusable pattern whenever a sibling session's dirty tree would
otherwise poison this task's scope gate: worktree-isolate, commit there,
supply the scope-check its diff explicitly, point every command step's `cwd`
at the worktree.

`source_event: task-01KXECD66GVAX33XXGY5F99FQ4, s_scope-check attempt1->attempt2, comment 01KXHFGPPE18HXKR1J26VN8NDV, 2026-07-14`

## 2. Integration goldens embed absolute repo-root paths — worktree isolation WILL spuriously fail them

`All/IntegrationHttpSpec.MatchesGolden/http_reindex` fails under any
worktree/relocated checkout even when the change under test is unrelated: the
golden's expected reindex message hard-codes the primary checkout's absolute
path (`.../lci-cpp/tests/parity/...`), so a worktree at a differently-named
path (`.../lci-cpp-langmap/...`) diffs only on the directory basename. Any
future worktree-isolated loop run WILL hit this same false failure on any
golden that echoes a filesystem path.

Follow-up recommended (not yet filed as a task): normalize the repo-root to a
placeholder token in golden capture *and* compare, once, workspace-wide —
same category as the `KDL for config / JSON for content` and Go-`nil`-slice
normalization precedents in `karpathy-principles.md` rule 6.

`source_event: task-01KXECD66GVAX33XXGY5F99FQ4, ctest-full-gate attempt1 stdout_tail, comment 01KXHFGPPE18HXKR1J26VN8NDV, 2026-07-14`

## 3. `task_workflow_step_force` is legitimate only for artifact-caused, change-unrelated gate failures — never for real defects

The `ctest-full-gate` step was closed on attempt 2 with an operator override
after attempt 1 failed on exactly the one golden described in lesson 2 (1995/1996
passed; the sole failure's diff was the checkout-directory basename, zero
relationship to the `detect_language` change under test). Force is
appropriate here because the failure is **proven orthogonal** to the change
(root-caused, diffed, and documented) and **not reproducible in a canonical
checkout**.

Contrast: if the failing assertion touched the changed code path, or the
root cause were undetermined, the correct move is the reopen/rewind path
(back to implementation), not force. Force without a documented, diffed root
cause is scope for silent defect-shipping — do not use it as a shortcut past
a gate that's merely inconvenient.

`source_event: task-01KXECD66GVAX33XXGY5F99FQ4, ctest-full-gate attempt1->attempt2 (outcomeJson null on override), comment 01KXHFGPPE18HXKR1J26VN8NDV, 2026-07-14`

## 4. A dirty tree whose files exactly match the resuming task's own `fileScope` is abandoned-in-flight residue, not a sibling conflict — recover by committing, don't worktree-isolate

Distinguish two dirty-tree shapes on task resume, they need opposite fixes:

- **Sibling conflict** (lesson 1 above): dirty paths fall OUTSIDE this task's
  `fileScope` → worktree-isolate, commit there, feed the scope-check an
  explicit diff.
- **Abandoned-in-flight** (this lesson): `status=doing`, `leaseHolder=null`,
  `leaseExpiresAt=null`, and every dirty path IS inside this task's own
  `fileScope` → a prior session wrote (possibly GREEN) work and never
  committed or closed. No worktree needed: the residue belongs here.
  Recovery is verify-then-commit in place — run the task's own test command
  against the dirty tree first (don't trust an uncommitted diff blind), then
  commit as a normal logical unit, then resume the remaining workflow steps
  from wherever they left off. Record the recovery as a task comment (lease
  state observed, which files, test result before commit) so a later reader
  doesn't have to re-derive it.
- A **live holder** is a non-null `leaseHolder`/`leaseExpiresAt` — do not
  touch that tree; this lesson and reference-port-discipline.md rule 9
  (work-evidence over renewal liveness) both apply only once the lease is
  confirmed dead.

`source_event: task-01KXEEH7RD3D03VN6EP8ZZEP0F ("S1 — Forge deterministic mutated exploration corpora"), workflow 01KXH30SW9CRPTKQ1CPPV89SRJ bench-unit-tests attempt1 (failed, ImportError, 2026-07-14T19:52) -> attempt2 (passed, 2026-07-14T20:48), comment 01KXH65CHP9GDD5670RMVSCY3X, 2026-07-14/15`
