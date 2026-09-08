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


When the gate finds it anyway, the coordinator repairs it directly rather than reopening
the implementation step: re-pin the golden itself, widen the task's `fileScope` by that one
file, and re-run `lci_integration_tests` STANDALONE on the same build as the gate. A
standalone pass on the unchanged binary is equivalent evidence to a second full `ctest`, so
rule 2's "exactly one full run" still holds. S7 did this for
`tests/integration/goldens/mcp/code_insight/basic.json` (`2c2ac87`, fabricated `tokens=90`
-> honest `tokens=225` after `0f12c41` changed what the LCF header means): 155/155
standalone, no second gate attempt spent.

`source_event (coordinator-repair clause): task-01M1NCSJ31DY6Q4ZBK6RS627E0, ctest-full-gate attempt1, comment 01M1ZB0TG3KXVW680C33J68FQW, commits 0f12c41/2c2ac87, 2026-09-08`

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
S9 extends this to the GATE itself: the same low-memory guard killed an in-harness
`ctest --output-on-failure -j4` with 34 GB free, and the run only completed detached
(`setsid nohup ctest ... > <scratchpad>/s9-final-gate.log &`, 2641/2641, exit 0). And an ACP
implementer's stall detector counts its own build wall-clock as silence — S9's attempt 2 was
killed `agent_stalled` after 15 minutes of a RED-proof worktree build under host load ~100,
having landed 3 of 4 items. A coordinator dispatching a slice that must build a second tree
either raises `stall_seconds` or tells the agent to detach the build.
<!-- written_at: 2026-09-08T06:30:00Z  source_event: task:01M1NCSJ31SW3FZE4FG0QRE1QY, comment:01M1ZRCHSGT80ECVZXWB5FVWT8, comment:01M1ZPJDTF2B1KF9K7Z4HZ0E65 -->

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

## 8. Any test that touches process environment or libc time/locale state uses `tests/helpers/portable_env.h` and skips on `_WIN32` — the Linux gate cannot see a Windows compile error

`ctest --test-dir build/release` proves nothing about MSVC. The Windows CI leg builds and
runs the FULL `lci_tests` binary (`.github/workflows/ci.yml:210-228`), so one unguarded
POSIX call in any test file reddens it long after the slice has closed — and the vendor
implementer, which builds Linux-only, cannot observe it at all.

S7's DST test called `setenv`/`unsetenv`/`tzset` bare and set `TZ=America/New_York`. The
MSVC CRT provides none of those three and reads `TZ` only as `tzn[+|-]hh[dzn]`, never an
IANA zone name, so the leg would not have compiled and could not have computed the
assertion if it had. The reviewer caught it by reading the CI file, not by running a test,
and fixed it in place (`cdd2883`): include the existing shim and `GTEST_SKIP()` on `_WIN32`
with the platform reason stated.

Rule: `setenv` / `unsetenv` / `tzset` / `setlocale` in a test go through
`tests/helpers/portable_env.h`. Where the behaviour itself is unavailable on the platform
(an IANA zone name here), skip with the reason rather than asserting something the CRT
cannot produce. A reviewer of any test-touching slice checks this against the CI file —
green Linux is not evidence.

`source_event: task-01M1NCSJ31DY6Q4ZBK6RS627E0, review-panel attempt1 advisory + comment 01M1ZCCSRXAE8WY63KJY0EJRVP (systemic, fix-now), commit cdd2883, .github/workflows/ci.yml:210-228, 2026-09-08`

## 7. A change to a classification or matching RULE ships with a before/after diff over the real corpora, and the RED pins BOTH directions

Rules 4 and 5 cover a changed output *value* meeting stale goldens. This is the tier above:
a change to the rule that DECIDES a category. Tightening one is not a subtraction of false
positives — it moves a boundary, and the true positives on the other side of it leave
silently, with every unit test still green because the tests name only the false positives
the change was written to remove.

S11 hit this twice on one function. `d0d05b7`/`85260f0` replaced bare prefix matching in
`classify_callee_category` with a word-boundary rule, tested against exactly the named FPs
(`querySelector`, `login`, `closest`, `listener`). The same rule refuses every leading-camel
compound, so Node's `fs` `*Sync` family (`readFileSync` 306 sites, `writeFileSync` 80,
`readdirSync` 58, `closeSync` 40, `mkdirSync` 37, `openSync`/`unlinkSync` 20 each in
next.js) and Go `ListenAndServe`/`DialContext` flipped from io/network to pure. `aac3e5e`
re-enumerated only the three compounds a hand audit had happened to name
(`MkdirAll`/`OpenFile`/`ReadAll`), leaving the `*Sync` family open. Review attempt 1 found
it and rewound. The rewind fix `052fc16` then replaced the enumeration with a rule — strip
one known decoration suffix (`sync|all|file|context|andserve`) and re-run the SAME boundary
matcher on the case-preserved stem — and review attempt 2 found that the new rule readmits a
smaller FP class (`<x>RequestContext` -> network, 9 next.js sites; `TestRecordQueryAll` ->
database), filed as `01M20BVY4HEWZKBA1E638GKHY7`.

Both classes were found the same way, and by the reviewer, not the test suite: a small
replica of the matcher run over the identifiers in
`benchmarks/repo-qa/.work/exploration`, boundary-only versus boundary-plus-stem, listing
every identifier whose category CHANGED. Seconds to run; the named-callee table (13
positives, 6 negatives) could not express either finding.

Rule, for any keyword/pattern/classification change under `src/analysis` (side-effect
categories, layer and module keywords, path gates):

- **Run the matcher replica over the real corpora before and after, and report the counts
  by category, both directions** — items that gained a category and items that lost one.
  A tightening with no reported losses has not been checked; it has been assumed.
- **The RED pins both directions in one test.** Positives kept (with the pre-fix reference
  values read from the pre-fix tree, `git show <sha>:<file>`, not from the current
  classifier) and false positives removed. A one-directional RED re-ships the regression —
  it is what shipped `aac3e5e`.
- **Prefer a rule over an exception list whenever the exceptions form a family.** A keyword
  table with a per-case exception list invites exactly this defect: the list holds whatever
  the last audit happened to see. A decoration suffix is a spelling habit, not a new word
  sense, so the stem carries the verb's meaning and the whole family closes at once. Pin one
  NEGATIVE per verb the rule touches (`openAccount`, `closeModal`, `readMemory`,
  `writeBuffer`, `listenerCount`, `dialogTitle`) so decay back into prefix matching is
  inexpressible rather than merely discouraged.
- **Re-run the corpus diff on the replacement rule too.** The principled rule is not exempt;
  it is where the second FP class came from.
- Watch the case-sensitivity seam: the boundary matcher's camel-tail arm keys on an
  uppercase byte, so any preprocessing that lowercases the callee before matching quietly
  disables it (`appendFileSync` -> `file` is lost).

The corpus-diff replica is not yet a committed script — it was written twice, by hand, by
the reviewer. Committing it under `benchmarks/` or `tests/` and naming it in the slice's
acceptance is the standing follow-up.

<!-- written_at: 2026-09-08T12:00:00Z  source_event: task:01M1NCSJ31593067Q5NPTV6D1X, comment:01M20AYRNZ36VC843M9MPEW08V (review a1 fail), comment:01M20AZ957KBX43CBB9FW3K1TX (failPatterns systemic), comment:01M20B7XQYYZ2PGBE4BR0MMZ3M (task_annotation decisions), comment:01M20CB1M1D693MTC4WCYSC0ZH (review a2 systemicObservations), git:85260f0, git:d0d05b7, git:aac3e5e, git:9c7a718, git:052fc16 -->

## 8. When a sanitizer preset is broken by a dependency, prove the memory-safety criterion with a standalone driver over the exact TU

`cmake --preset sanitizer` cannot compile abseil under this host's GCC (pre-existing absl
constexpr failure; filed `01M20CE5HNBGD4DEM9RH34ZA52`). A memory-safety acceptance criterion
that names the preset is then unsatisfiable, and the cost of fixing the dependency is not the
slice's to pay.

Recipe, proven on S11 criterion 10 and reproduced independently by the reviewer from the
recorded command:

1. Write a driver `.cpp` that `#include`s the implementation `.cpp` itself, so a function in
   an anonymous namespace is reachable in the same TU.
2. `g++ -std=c++20 -fsanitize=address -I include -I build/release/generated` plus `-isystem`
   for each dep source tree, linking `build/release/src/liblci_lib.a` and the release dep
   archives inside `-Wl,--start-group ... --end-group`.
3. Run it against the PRE-fix source (`git show <red-sha>^:<file> > /tmp/...` and include
   that copy) AND against HEAD. Post the exact command and BOTH outputs as a task comment.
4. If the pre-fix run does NOT report a violation, say so plainly. A bounded buffer stands on
   its own; an invented sanitizer report does not.

Here the pre-fix run confirmed a previously UNVERIFIED root-cause claim — `AddressSanitizer:
stack-buffer-overflow, WRITE of size 4 in edit_distance_capped`, frame object `prev`
(naming_analyzer.cpp:135) `[160,416)` overflowed at offset 416 — and HEAD ran clean. A
root-cause the task marks unverified is a hypothesis; this is how it becomes evidence, and
the standalone driver costs minutes against the hours of fixing a dependency under
`-fsanitize`.

<!-- written_at: 2026-09-08T12:00:00Z  source_event: task:01M1NCSJ31593067Q5NPTV6D1X, comment:01M2088YG2JX0N4MPKG62XXKND (ASan evidence), comment:01M20AZ957KBX43CBB9FW3K1TX (passPatterns), git:b61e5e3, git:aefcd1e, task:01M20CE5HNBGD4DEM9RH34ZA52 -->
