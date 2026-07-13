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
