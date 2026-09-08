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

- **A capped/timed-out agent attempt produces the same shape and recovers the same way.** S4's
  first ACP attempt hit the 7200 s cap mid-refactor with six commits landed and a 4-file, +45/-44
  in-scope diff uncommitted. Two halves of the fix: (a) the coordinator reopens as
  `transient_environment` and PREPENDS a resume block to the task content naming every landed sha
  with its subject, the uncommitted paths with their `--stat`, and the criteria still outstanding
  — a resume without it re-derives or redoes work; (b) the implementer commits each criterion's
  GREEN before opening the next one, so a cap loses at most one criterion instead of a refactor.
  Attempt 2 opened by re-reading the `git diff`, running the targeted tests, and committing the
  residue as `5b05997` before touching anything new; that is the correct order under this rule.

`source_event: task-01KXEEH7RD3D03VN6EP8ZZEP0F ("S1 — Forge deterministic mutated exploration corpora"), workflow 01KXH30SW9CRPTKQ1CPPV89SRJ bench-unit-tests attempt1 (failed, ImportError, 2026-07-14T19:52) -> attempt2 (passed, 2026-07-14T20:48), comment 01KXH65CHP9GDD5670RMVSCY3X, 2026-07-14/15`

## 5. Never `git checkout` the shared primary checkout onto a feature branch — add a worktree instead

Rule 1 covers a sibling's dirty tree poisoning YOUR scope gate. This is the mirror: YOU
poisoning every sibling session. The primary checkout at
`/home/beagle/work/core/lci-cpp` is shared by concurrent loop sessions that all assume
`main`. Switching its branch silently relocates every sibling's build tree, `ctest`
target, and scope-gate diff base, and nothing reports it — the sibling just starts
failing gates against code it never wrote.

D4's implementer switched the primary checkout to `worktrack/d4-sweep` to build on an
unmerged chain. The correct move for the same need is rule 1's recipe used
proactively: `git worktree add ../lci-cpp-<slug> <branch>`, implement there, and point
every command step's `cwd` at the worktree. A branch switch of the shared checkout is
never in a task's `fileScope` and never needs to be.
<!-- written_at: 2026-09-06T17:00:00Z  source_event: task:01KXQ2V3P299T64XZ6BXS2QMH1, comment:01M1VQREGZM95YZ2VP0E96VN9S, comment:01M1VQV19V92QMV155QYPZMJ4X, git:945e50a -->

## 5. A planning artefact that later slices depend on is COMMITTED in the planning commit — never referenced by session-scratchpad path

A session scratchpad (`/tmp/claude-*/.../scratchpad/`) is purged between sessions.
An epic body that points at one for a load-bearing artefact has no copy of it once
the planning session ends, and every downstream slice inherits the loss.

Evidence: the review-drain epic (`01M1NCA90N4VPDKP4XR42SE8V8`) named
`/tmp/.../scratchpad/review-draft.md` as the only copy of its 47-finding review
draft. By the time S0 (`01M1NCSJ31ZEV9XGETA6VAF8CK`) ran, the file was gone
(`find /tmp/claude-1000 -name 'review-draft*'` empty, `git log --all -- docs/reviews/*`
empty). S0's first criterion had to be rewritten mid-flight (coordinator decision
`01M1WR81BGRDGG78KWDQ6TK9AM`) and its first commit `0d99447` spent on reconstructing
the document from the epic + child bodies instead of on the slice's actual subject.

Rule: any artefact a task body cites as a source — review draft, port map, decision
record, corpus manifest — is committed under `docs/` (e.g. `docs/reviews/`,
`docs/plans/`) in the same commit that creates the tasks, and cited by repo-relative
path. A task body containing an absolute `/tmp` path is a planning defect: fix it at
planning time, not at drain time. Scratchpad paths are legitimate only for material
no other session needs.
<!-- written_at: 2026-09-07T04:00:00Z  source_event: task:01M1NCSJ31ZEV9XGETA6VAF8CK, comment:01M1WR81BGRDGG78KWDQ6TK9AM, git:0d99447 -->

## 6. Verify a RED test in a fresh detached worktree, not in the tree that holds the fix

Standard C++ review shape when checking that a pinned RED test really fails without the
fix: `git worktree add --detach <scratchpad>/red-<slug> <pre-fix-sha>`, cherry-pick the
RED commit alone, build only `lci_tests` (the shared `FETCHCONTENT_BASE_DIR` dep cache
and ccache make this cheap — see the worktree shared-dep-cache note), and run the single
`--gtest_filter`. Re-running the RED test in the working tree proves nothing once the fix
is present, and stashing the fix pollutes a tree another session may hold.

Remove it with `git worktree remove` when done, and know the hazard that follows: a
worktree configured against the shared `FETCHCONTENT_BASE_DIR` leaves `*-build` Makefiles
in `~/.cache/lci-cpp-deps/release/` pointing at its now-deleted path, so the NEXT session's
build fails resolving paths under the dead worktree. Fix: delete the `*-build` dirs (keep
`*-src`) and reconfigure. S7's implementer spent part of its window doing exactly that for
12 dep build dirs.
Since S9 the repair is one line: `cmake --preset release` in the PRIMARY checkout, confirmed
by `grep -l <worktree-slug> ~/.cache/lci-cpp-deps/release/*-build/Makefile | wc -l` = 0. It is
the mandatory second half of the procedure, not a follow-up — S9 paid it twice in one task
(the ACP implementer and the reviewer each re-pointed 11 Makefiles).

Two more things the S9 RED proof cost, both cheap to avoid:

- **Carry the helpers the slice introduced.** The RED commit's cherry-pick conflicts when the
  test calls a helper the same slice added (`ensure_lci_server_indexed`, added in `f893f10`),
  and the first build then fails on an undeclared identifier. Copy the test AND every
  slice-introduced helper it calls, inside the same namespace. Three builds were wasted across
  two actors here before this was understood.
- **The RED's reference value must come from a source OUTSIDE the binary under test.** S9's
  `CliStatusTest.ThreadsAndRssMatchServerStatusJson` compared `lci status` text against `lci
  status --json`; pre-fix both read the CLI's own `/proc/self`, so the two agreed and the RED
  passed on the pre-fix binary — the commit claimed 5/5 RED, the truth was 4/5. Rewritten to
  read the server in-process via `lci::Client(get_socket_path_for_root(root)).get_stats()`, it
  fails pre-fix by 12 MB of RSS. This is `karpathy-principles.md` rule 4's two-producers /
  one-stream class applied to a CLI test: a test whose claim is "this value comes from X, not
  from the process itself" must obtain X independently.
- **Build the RED worktree detached and at `-j2` while a sibling session holds the host.** S10's
  reviewer lost a `-j6` build to the host memory guard (killed mid-link); the same target built
  fine detached at `-j2`. The RED proof needs one binary, not a fast one.
- **Paste the per-test `[  FAILED  ]` lines, never a count.** "RED: all N fail" is a
  load-bearing claim and only the gtest output proves it; the false count is exactly what hid
  the non-discriminating test through a whole review cycle.

<!-- written_at: 2026-09-08T06:30:00Z  source_event: task:01M1NCSJ31SW3FZE4FG0QRE1QY, comment:01M1ZK4ESZB6KNSY4KX6NZ4C7K (review a1 failPatterns), comment:01M1ZSXHPQBR9C1AG1MMVXZ32T (review a2 systemicObservations), comment:01M1ZQE8Q3DNGRDYR35N8SNR4M (b2_red_result), git:b8e9a27 -->
<!-- written_at: 2026-09-07T06:30:00Z  source_event: task:01M1NCSJ315Z7470JQWY24DF1V, review verdict 01M1X3JC399NZE2SXAV2WRJB5W -->

## 7. A rewind's scope widening must cover every file the fix hint IMPLIES, headers included

The reviewer's `scopeChanges.new_scope` lists the files named in the blocker
(`watcher.cpp`, `master_index.cpp`, `master_index_test.cpp`). A fix that changes a locking
contract also changes the header that DOCUMENTS it, and that header is not in the list —
S3's attempt-2 scope gate took three attempts, the third only to admit
`include/lci/indexing/master_index.h` for the invariant comment beside the widened lock.

Rule: when writing `new_scope` at rewind, include the declaring header for any signature,
lock-order, or invariant comment the fix hint touches. At drain time, a scope failure whose
only excess path is the header of an in-scope `.cpp` is a scope-authoring miss, not a
discipline breach — widen and re-evaluate rather than routing the doc edit elsewhere.

<!-- written_at: 2026-09-07T16:10:00Z  source_event: task:01M1NCSJ31A8XHPC7NKJEGS7RV, comment:01M1XARQG97QQRBYFEC4M06YBJ (scopeChanges.new_scope), comment:01M1Y9F94319CA8P3SN338J7ZA (scope a3), git:734ced3 -->

## 7. A criterion whose RED needs a hook the code does not expose is closed by inspection plus a disclosed guard test — never by a test that would have passed anyway, silently

Rule 6 says prove RED in a fresh worktree. Some criteria have no deterministic RED at all: S4's
"umask(077) before bind" guards the window between `bind_to_port` and the later `chmod`, and
observing a socket's mode inside that window needs a hook httplib does not offer. The shipped
`SocketModeIsOwnerOnlyUnderPermissiveUmask` passes on the pre-fix tree via that later `chmod`.

That is acceptable, under three conditions, and unacceptable without them:

1. **The implementer DISCLOSES it at commit time and in the task comment** — naming which test is
   a true RED and which is a guard that passes pre-fix, and why no RED exists. Here `d622ca9`
   carried both: `SocketLockSymlinkIsNotFollowed` is a true RED (pre-fix the server flocked the
   symlink's target); the umask test is the disclosed guard.
2. **The reviewer records the acceptance-by-inspection in the verdict**, naming what was inspected
   (`umask(0077)` scoped around `bind_to_port` and restored) — so the exception is auditable
   instead of inferred from a green suite.
3. **The guard test still ships.** It pins the window closed against a future edit that reopens it,
   which is the only thing it can do. A criterion with neither a RED nor a guard is unproven.

An undisclosed guard test is the failure this rule exists to stop: it reads exactly like a RED in
the log, and the next reader has no way to tell the difference.

Sibling shape, same discipline: the criterion's failure SCENARIO is unreproducible while the
DEFECT is real. S10's criterion (a) asked for "two concurrent `run_capture`, one child sleeps
2 s, the other returns in < 1 s". Pre-fix, `posix_spawn` file actions closed both ends in the
spawning child and the parent closed the write end immediately, so only the READ end leaked —
a pure fd leak with no blocking, and the 2 s shape needed a microsecond window that never
reproduced. The implementer pinned the MECHANISM instead with an independent kernel-observable
(`/proc/self/fd` count against an ambient baseline: 5 vs 4 pre-fix, deterministic under load)
and disclosed the substitution with its reason; the reviewer verified the mechanism claim
against the PRE-FIX code rather than re-litigating the scenario prose. Rule: when a scenario
cannot be made to fail, do not weaken it into a test that passes either way — pin the leaked
or corrupted resource with an observable outside the code under test, disclose the swap at
commit time and in the task comment, and have the reviewer check the substitution against the
pre-fix source. A concurrency criterion whose window is narrower than the scheduler is a
scenario, not a test.

`source_event: task-01M1NCSJ31JA7K2WZJY5DGASHZ, comment 01M1YN2KMVDWG69MFYKE2ZWYQ4, verdict 01M1YV33SDYRC4FYQA2NVNPMGW (umaskDecision), git d622ca9/120fe3b, 2026-09-07`
<!-- written_at: 2026-09-08T08:30:00Z  source_event: task:01M1NCSJ31H91WK6XGBGSTVK9X, verdict:01M200MSXF1QTF9TF31ZTTXVMK (deviation1), comment:01M200N3D0KANTKHFM9CVEG0QW (passPatterns[0]), git:d55a5ad, git:711c8a8, git:76bd21e -->

## 7. An INTEGRATION-POINTS claim that a symbol is reusable from the slice's scope must be PROBED at planning time, and the declaring header listed in `fileScope`

Rule 5 governs an artefact a task body cites. This is the same failure for a SYMBOL a
task body cites: S6's body said "the wildcard_match already in handlers_core — reuse it,
do not write a second matcher", and the file scope was written around that sentence.
`wildcard_match` was in fact defined in an anonymous namespace at
`src/mcp/handlers_find_files.cpp:37` with no declaration in any header, so it was
unreachable from every file the slice was allowed to touch. The ACP implementer stopped
on that criterion correctly — the only alternatives were duplicating the matcher or
editing out-of-scope files — and the criterion cost a coordinator scope widening by two
files (`include/lci/mcp/handlers_core_shared.h`, `src/mcp/handlers_find_files.cpp`) plus
a second implementer dispatch after the ACP attempts were exhausted.

This is the presence-side twin of `bench-harness-oracle-independence.md` rule 10 (an
absence claim must cite its probe): a REACHABILITY claim is load-bearing because the
whole scope is designed around it, so it carries the same evidence bar.

Rule: before writing "reuse X" into a task body, run the probe — `lci def <symbol>` or
`grep -rn '<symbol>' include/` — and record what it found. If the definition sits in an
anonymous namespace or a `.cpp` with no declaration, the slice's scope MUST include the
header that will declare it and the TU that defines it, or the promotion must be its own
preparatory slice. An unprobed "already exists in <module>" is a planning defect that
surfaces only after the implementer has spent its attempts.

Corollary paid for in the same slice: promoting a helper into a shared header can break
the build through ambiguous overloads, because handler TUs carry private copies of that
header's inline helpers (`to_lower`, `clamp_int` in `handlers_explore.cpp`). Deleting the
local copy is part of the promotion, not a surprise.

`source_event: task-01M1NCSJ31XWSRVAPP3A3YQX5Z, acp-implement attempt2 (passed end_turn, criterion 1 stopped), comment 01M1Z2Z4Y1T9NS7Q6A099ZHWKV (coordinator decision), comment 01M1Z39J8Z42QFZ5FEVKWX89HV (task_annotation lessons), commits 6b127d5/04457f3, 2026-09-07`

## 8. Order a multi-criterion slice cheap-and-deterministic first, and commit each RED and each GREEN before the next criterion

An ACP implementer runs under a hard wall-clock cap. When the dispatch prompt orders the
criteria itself — cheap deterministic fixes first, the measured/perf criterion last — and
requires a commit at each RED and each GREEN before the next criterion is touched, a
timeout loses one criterion instead of the slice.

S7 carried 8 criteria over 20 files and finished in ONE attempt: 168 tool calls, 81 min of
a 120-min cap, 14 commits (`9ff5f32..15f8dc7`) in RED/GREEN pairs, `end_turn`, every
criterion green. The ordering was written into the step's prompt, not left to the agent.

S10 repeated it: 7 criteria over 10 files, ONE attempt — 150 tool calls, 76 min, 15 commits
(`d55a5ad..76bd21e`) in RED/GREEN pairs plus three cleanups, `end_turn`, full gate 2649/2649
with no force. Its one extra knob was `stall_seconds` raised to 1500, because several criteria
build and run a real git fixture repo between tool calls.

`source_event: task-01M1NCSJ31DY6Q4ZBK6RS627E0, acp-implement attempt1 (passed, end_turn, duration_ms 4888849 of timeout_seconds 7200), workflow 01M1Z5DNT8FXBCPK0RPND7A615 specJson ORDER OF WORK, comment 01M1ZCE5QJSVEX6AGJNB86FWRZ, 2026-09-08`
<!-- written_at: 2026-09-08T08:30:00Z  source_event: task:01M1NCSJ31H91WK6XGBGSTVK9X, comment:01M200Q2G3Z4K5851DJ71FAJVF (coordinator checkpoint), git:d55a5ad..76bd21e -->

Third data point, and it sets a size ceiling: S11 carried 10 criteria over 20 files and did
NOT fit. Its ACP attempt 1 ran the ordering correctly — cheap deterministic criteria first,
a commit at each RED and each GREEN — and still hit the 2 h cap mid-way through criterion
10's ASan probe, having landed 19 commits (`3afa248..b13892d` plus the criterion-10 RED
`b61e5e3`). Attempt 2 finished the one remaining criterion in 8 minutes from a RESUME body
that listed what had already landed. Nothing was lost, because the ordering held; the slice
simply exceeded one attempt.

So the ordering rule buys graceful degradation, not unlimited size. With S7 (8 criteria,
81 min of 120) and S10 (7 criteria, 76 min) fitting and S11 (10) not, a planner should cap an
ACP slice at about 8 criteria and split beyond that. When an attempt does run out, the
RESUME body is the recovery: name every criterion already landed with its commit range and
"do NOT redo", leave only the remainder, and inline the exact command for any probe the next
attempt must run.

<!-- written_at: 2026-09-08T12:00:00Z  source_event: task:01M1NCSJ31593067Q5NPTV6D1X, comment:01M20CBY8TV1VHA8X3K212DHPZ (coordinator checkpoint), task content (RESUME preamble), git:3afa248..b13892d, git:aefcd1e -->
