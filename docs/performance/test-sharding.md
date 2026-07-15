# CTest discovery granularity for `lci_tests` (S2)

Task `01KXEEWQG43DRYADAT32XT9RRH` (parent `01KXDNR82BSBQMYGRZPWDK4H35`).
Follows S1 baseline `docs/performance/test-suite-baseline.md`.

**Decision: NO CMake change — keep `gtest_discover_tests` in the default
`POST_BUILD` discovery mode.** Every measured alternative that reclaims the
per-process spawn overhead does so only by collapsing the 1851 per-test ctest
entries into a coarser granularity, which breaks the PRESERVE constraints
(`ctest -R <TestName>` selection, per-test GoogleTest failure names, per-test
`TIMEOUT`). This is a valid measured no-change close.

## Environment

Same checkout / build as S1: branch `worktrack/test-suite-perf`, `build/release`
(Release, GNU c++, cmake/ctest 3.28.3), warm global ccache (~84% hit rate),
12 CPUs, `ctest -j4`. All numbers measured on this checkout.

## Current configuration (baseline for this slice)

| metric | value |
| --- | --- |
| discovery mechanism | `gtest_discover_tests(lci_tests ...)` |
| discovery mode | `POST_BUILD` (default; `CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE` unset) |
| ctest entries — `lci_tests` (label `unit`) | **1851** (one process per test) |
| ctest entries — total suite | 1996 |
| `ctest -R ClientTest.WaitForReadyTimeout` | selects exactly **1** entry |
| build-time discovery cost (`lci_tests --gtest_list_tests`) | **~0.00s** (negligible) |
| in-process body time (one process, all unit tests) | **5.39s** |
| unit-suite wall `ctest -j4 -L unit` (median of 3) | **~7.3s** (7.30 / 7.32 / 11.36) |

### Process-launch overhead (from S1, restated)

Summed CTest per-entry wall across the 1851 processes = **28.0s**, vs **5.6s** of
gtest body time when the same tests run in one process → **22.4s (79.9%,
~12.1 ms/entry)** is pure per-process spawn + per-process global init/discovery,
not test-body work. Under `ctest -j4` this overhead is parallelized across the
worker pool, so the *wall* penalty over a single bundled process is only
~1.8–1.9s (7.3s vs 5.4s), not the full 28.0s.

## Alternatives benchmarked (identical warm-ccache settings)

### A. `DISCOVERY_MODE PRE_TEST`

Moves test enumeration from build time to ctest-invocation time. Reconfigured,
rebuilt, verified entry count (1851, unchanged) and `-R` selection (1, preserved).

| unit-suite wall `ctest -j4 -L unit` | 8.65 / 9.07 / 11.07s — **median ~9.07s** |
| --- | --- |

**Slower, not faster.** PRE_TEST re-runs `--gtest_list_tests` on every ctest
invocation and does not reduce per-test process spawn during the run. Rejected —
fails the "measurably faster" bar.

### B. Coarse granularity — single bundled `add_test(lci_unit_suite COMMAND lci_tests)`

Collapses the 1851 per-test entries into ONE ctest entry running the whole
binary in a single process.

| metric | value |
| --- | --- |
| ctest entries (label `unit`) | **1** (down from 1851) |
| unit-suite wall `ctest -j4 -L unit` (median of 3) | **~5.54s** (5.51 / 5.54 / 9.09) |
| reclaimed wall vs current | ~1.8s at `-j4` (matches the 5.39s in-process floor) |
| `ctest -R ClientTest.WaitForReadyTimeout` | **0 entries — SELECTION BROKEN** |
| per-test GoogleTest failure names in ctest | lost (ctest sees one exit code) |
| per-test `TIMEOUT 60` | lost (collapses to one bundle timeout; a single hung fixture blocks the whole binary — the exact failure mode the `TIMEOUT 60` comment guards against) |

**Rejected.** It is the only configuration that reclaims the spawn overhead, but
it violates every PRESERVE constraint: `ctest -R <TestName>` returns zero
matches, per-test failure attribution disappears, and the per-test 60s cap that
guards against ServerTest fixture hangs is gone. Per the acceptance criteria,
"If a coarser mode breaks `ctest -R <TestName>` selection, it's not acceptable —
keep the finer mode."

## Keep / revert decision

**KEEP the current `POST_BUILD` per-test discovery — no CMake change committed.**

- The spawn overhead (22.4s summed; ~1.8s at `-j4` wall) is inherent to having
  one ctest process per test. The only lever that removes it (coarse bundling)
  breaks selection, failure names, and per-test timeouts.
- `POST_BUILD` is already the correct discovery mode: enumeration is ~0.00s and
  happens once at build; `PRE_TEST` only adds per-invocation latency.
- Real integration/real-project cost lives in the RUN_SERIAL bundles
  (`lci_integration_suite` 272.9s, `lci_real_project_suite` 26.9s) — S3's surface,
  not discovery granularity.

## Parallel-collision check (selected config, 10× consecutive)

`ctest -j4 -L unit`, 10 consecutive runs on the unchanged config:

**10/10 PASS**, `100% tests passed, 0 failed out of 1851` each run, wall
7.72–9.36s. No shared path / socket / port / index-dir / teardown collision
surfaced. The existing isolation helpers already make every parallel worker
process-unique:

- `tests/helpers/test_socket.h` — `next_test_server_address()` folds the pid
  into the socket filename (POSIX) / port base (Windows).
- `tests/helpers/unique_temp.h` — `unique_suffix()` folds the pid into every
  scratch temp path.

No helper edit was required; both files are left unchanged.

## Before / after summary

| | before | after |
| --- | --- | --- |
| discovery mode | POST_BUILD | POST_BUILD (unchanged) |
| unit ctest entries | 1851 | 1851 |
| `ctest -R <TestName>` | 1 | 1 |
| unit wall `-j4` (median) | ~7.3s | ~7.3s |
| spawn overhead (summed / `-j4` wall) | 22.4s / ~1.8s | 22.4s / ~1.8s |
| 10× race check | — | 10/10 green |

Net: no change. The reclaimable ~1.8s wall was rejected because it costs
per-test selection, failure names, and timeouts — a bad trade for a warm suite
that is already ~7.3s.
