# Test + build iteration discipline (Karpathy rule 7)

Audit 2026-07-13: 77% of ctest invocations ran the full suite (~420s each, ~2.9h
of a single loop session); 80 full cmake rebuilds ~1.9h. Almost all were
iteration loops, not gates.

## Rules

1. **Iterating** (red-green cycles, debugging a failure):
   - Build only what you run: `cmake --build build/release --parallel --target lci_tests`
     (or `lci_helpers_tests`, or the one binary you need). ccache is wired
     automatically in CMakeLists.txt — never bypass it with clean rebuilds.
   - Run only the module under change: `ctest --test-dir build/release -R <TestSuiteName> -j4`
     or the gtest binary directly with `--gtest_filter='<Suite>.*'` (faster: skips ctest overhead).
2. **Gate** (workflow ctest step / pre-commit final check): full suite, exactly once,
   `ctest --test-dir build/release --output-on-failure -j4`. Full build precedes it.
   Workflow templates `cpp-slice-v2` / `cpp-perf-v3` encode this — do not hand-run
   extra full suites around the gate.
3. **Commit rule unchanged**: every commit still requires the full suite green —
   satisfy it with the single gate run, not repeated ad-hoc full runs.

## Code navigation: dogfood lci

This repo BUILDS lci. Use it for code nav instead of grep/rg where it fits:
`build/release/src/lci search|def|refs|context <query> -r .` — it is the product;
every loop session is a test session. Fall back to Grep only when lci lacks the
capability (and note the gap as a finding).

## Subagent toolsets

Implementer/reviewer subagent dispatches MUST include the Grep tool in their
toolset (5 dispatches in the audited session lacked it and burned turns
shell-catting files). Default: full toolset.

## 4. A change to a repo-wide output contract must grep `tests/integration/goldens/` before the gate

Targeted `ctest -R <Suite>` never runs the integration golden suites, and under the
snackable-member / container-gate split (`.worktrack/s12-decomposition.md`) member
slices run ONLY targeted tests. So a member that changes an emitted field repo-wide
re-pins the unit tests it can see and leaves the goldens encoding the old contract.
The failure then surfaces at some LATER task's full gate, charged to a change that
did not cause it.

Evidence: `ecd810c` (column becomes a 0-based byte offset repo-wide) and `f3662ce`
(invert rows carry `kColumnUnknown` instead of a fake 0) re-pinned
`tests/cli_test.cpp` and `include/lci/cli/column.h` and touched no golden. Two
`grep_compat` goldens kept the pre-`ecd810c` column base until an unrelated task's
`ctest-full-gate` failed on them (attempt 1, 2698 tests, 2 failed), costing a build +
full-suite cycle and a re-pin commit (`5ebad10`).

Rule: when a slice changes what any emitted field MEANS — not just its value —
`grep -rl '<field>' tests/integration/goldens/` before declaring the slice done, and
either re-pin in the same commit or name the goldens you are deferring. Do not rely
on the container gate to discover it; the gate reports the failure without the
context that explains it.
<!-- written_at: 2026-09-05T20:00:00Z  source_event: task:01M1PTEH33DW573D3R5J2N00PS, git:ecd810c, git:f3662ce, git:5ebad10, workflow-step:ctest-full-gate attempt1 -->

## 5. Run `lci_integration_suite` as a cheap pre-gate before the full `ctest`

The full gate costs 545-840 s wall; `lci_integration_suite` alone costs ~53 s and
holds every golden. Stale goldens are the failure class a container gate is most
likely to meet first, because members run only targeted `ctest -R` (rule 4) — so the
expensive run is spent discovering something the 53-second run already knew.

Evidence: S12's `ctest-full-gate` attempt 1 spent 555 s to report 3 failures, all of
them CLI goldens still pinning defects S12.5 had fixed
(`cli_symbols_browse_stats`, `cli_symbols_browse_stats_json`, `cli_symbols_refs_json`;
re-pinned in `496ab2d` + `1b24cab`). `lci_integration_suite` was the only failing
ctest entry — 2712 of 2715 other tests ran for nothing. The same class had already
cost a full cycle one task earlier (`5ebad10`).

Rule: a container/epic gate runs
`ctest --test-dir build/release -R lci_integration_suite --output-on-failure` first,
and only on green proceeds to the full `-j4` suite. A golden failure then costs 53 s
and names itself. This does NOT add a full run — rule 2 still allows exactly one.
<!-- written_at: 2026-09-05T23:30:00Z  source_event: task:01M1NCSJ31EQ7WZ9GEEB8CDCY5, workflow-step:ctest-full-gate attempt1 (555s, 3 golden failures) vs attempt3 (lci_integration_suite 53.28s), git:496ab2d, git:1b24cab, git:5ebad10 -->

## 6. A gate suite must not contain an absolute wall-clock assertion

`RealProjectSearchLatencyTest.FastapiSearchUnder5ms` asserts an absolute bound and
runs inside the `-j4` gate. Under host load from sibling sessions it failed attempts
2 and 3 of S12's gate as the *sole* failure (2714/2715), then passed 3/3 in isolation
at the same load — costing 1386 s of gate wall-clock and a `forced_v1` override that
had to argue orthogonality from the diff.

Until the test is made contention-robust (best-of-K min, or a relative-scaling ratio
— owner task `01KXMV1BXH6K5DRXGN3FVA1DQV`), a gate failure whose only entry is a
latency assertion is a load artifact, not a regression: confirm by re-running that
one filter in isolation, then force per
`worktree-isolation-and-goldens.md` rule 3. Do not spend a gate attempt on it.
<!-- written_at: 2026-09-05T23:30:00Z  source_event: task:01M1NCSJ31EQ7WZ9GEEB8CDCY5, workflow-step:ctest-full-gate attempt2+attempt3, comment:01M1SS0ETX44R3301Z74V07SSM, memory:contention-robust-perf-tests -->

### 6a. Force only when the diff provably cannot REACH the timed path; adjacency demands a pre/post A/B

Rule 6 lets you charge a lone latency failure to host load. That shortcut is only sound when
the slice's diff is nowhere near the timed subsystem. S4 was the adjacent case: it rewrote the
`/search` handler's lock path, and `FastapiSearchUnder5ms` failed at load 26 with its best-of-5
cluster sitting ON the bound (9485-10096 us against 10000 us), not far above it. A rerun would
have proved nothing either way.

The triage that settled it, and the recipe to repeat:

1. **Read what the test actually times.** Here `ctx.search -> MasterIndex::search_with_options`
   runs IN-PROCESS (`tests/integration/real_project_performance_test.cpp:95-111`,
   `tests/helpers/real_project_helpers.h:217-229`) with no `IndexServer`, socket or HTTP in the
   timed window — so a server-handler diff cannot reach it. State the reachability argument by
   naming the timed call chain and the diff's paths, not by asserting orthogonality.
2. **Interleave pre/post, do not batch.** Detach a worktree at the pre-slice sha, build both,
   and alternate runs so both binaries see the same load. Report min AND median:
   pre-S4 `c9b7e32` min 5651 / median 7720 us vs main min 5456 / median 7099 us, load 17.3-17.7,
   and both >10 ms samples landed on the PRE-slice binary. A post-slice binary that is faster on
   both statistics closes the question; a batched A/B cannot, because load drifts between batches.
3. **Record the numbers in the force reason.** `forced_v1` with "it's rule 6" and no bisect is
   the shortcut this clause exists to stop.

Build note for step 2: the harness low-memory guard killed the bisect build three times while a
sibling `rustc` peak ran. `setsid nohup <build> &` survives it — a detached build is the only one
that finishes under a contended host, and a foreground build under those conditions is a wasted
20-minute cycle, not a signal.

`source_event: task-01M1NCSJ31JA7K2WZJY5DGASHZ, ctest-full-gate attempt1 (2727/2728) -> attempt2 forced_v1, comment 01M1YRESKBG0JB47FFJ3BDZFRN, git c9b7e32..120fe3b, 2026-09-07`

## 7. A slice that edits an MCP tool schema or description must run the bench tool-surface gate — ctest cannot see it

Rule 4 makes a repo-wide output-contract change grep `tests/integration/goldens/`. The
same reasoning reaches one gate further out: `benchmarks/repo-qa/comprehension/surface/tool-surface.json`
is a committed snapshot of the live `tools/list` surface (see
`bench-harness-oracle-independence.md` rule 8a), it has 14 readers under `benchmarks/`,
and it is asserted by a **pytest** suite that no ctest target runs.

Evidence: S6's `7949bc6` removed `browse_file.show_imports` and `inspect_symbol.max_depth`
from the schemas. Scope, build, the full 2739-test ctest gate and the review all passed;
`pytest benchmarks/repo-qa/tests/test_tool_surface.py` has been RED on `main` ever since
(1 failed / 11 passed), and every tool-calling bench built on that snapshot measures a
surface the binary no longer serves. Only the reviewer's own probe found it
(filed `01M1Z59FVM4540T15VX7TTBMJD`).

Rule: any slice touching an `add_tool` schema or description runs
`pytest benchmarks/repo-qa/tests/test_tool_surface.py` as a pre-gate, next to the
`lci_integration_suite` pre-gate of rule 5, and re-pins the snapshot plus
`docs/TOOLS.md` in the same commit — or names the follow-up. Green ctest is not evidence
about a contract whose consumers live outside ctest.

Follow-up (not done here — it is a worktrack template change, not a repo edit): the
`cpp-acp-slice` template should gain this pytest command step for schema-touching slices,
or the C++ gate should acquire a ctest entry that shells the bench suite so the two
cannot diverge silently.

`source_event: task-01M1NCSJ31XWSRVAPP3A3YQX5Z, review_annotation_v1 01M1Z5AMKTBAS9AGVRMQDYQSEN (systemicObservations, frequency 14), verdict 01M1Z5A8SSSFQWW6DTJNC6HTJ8 advisory, git:7949bc6, follow-up 01M1Z59FVM4540T15VX7TTBMJD, 2026-09-07`
