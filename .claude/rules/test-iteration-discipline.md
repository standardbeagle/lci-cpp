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
