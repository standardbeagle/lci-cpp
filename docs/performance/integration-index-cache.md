# S3 — Content-addressed reuse of test-corpus indexing: reachability analysis

Task `01KXEEWQGK` (parent `01KXDNR82BSBQMYGRZPWDK4H35`). Branch
`worktrack/test-suite-perf`. Prove-before-build slice: Step 0 measurement gates
whether any cache is written.

**Verdict: measured no-change close (fork b + c).** No cache code is written.
The reachable indexing cost is already amortized by the existing in-process
cache; the one remaining win is out of this slice's file scope. A
scope-expansion proposal for S4 is recorded below.

File scope for this slice: `docs/performance/integration-index-cache.md`,
`tests/helpers/real_project_helpers.h`, `tests/helpers/real_project_index_cache.h`,
`tests/helpers/test_helpers_test.cpp`. Only this doc changed.

---

## Step 0 — Reachability (which top-10 cost does this scope reach?)

S1 baseline (`docs/performance/test-suite-baseline.md`, median gate **376.2s**):

| top-10 entry | wall | share | binary | indexing path |
| --- | --- | --- | --- | --- |
| `lci_integration_suite` | 272.9s | 67.5% | `lci_integration_tests` | **OUT OF SCOPE** — `spec_runner.cpp` reads `LCI_CPP` and drives the real `lci` CLI, which auto-spawns a detached per-corpus server daemon that indexes in a **subprocess**. Never touches `real_project_helpers.h` / in-process `MasterIndex`. |
| `lci_benchmarks` | 75.8s | 18.7% | `lci_benchmarks` | out of scope (benchmark binary) |
| `lci_real_project_suite` | 26.9s | 6.7% | `lci_real_project_tests` | **IN SCOPE** — the only binary that includes `real_project_helpers.h` and indexes via in-process `MasterIndex` through `setup_real_project`. |

Confirmed by source (`tests/CMakeLists.txt`, header grep):

- `real_project_helpers.h` is included **only** by the `real_project_*.cpp`
  files, which compile into `lci_real_project_tests` (26.9s). None of the six
  `lci_integration_tests` sources (`pipeline_integration_test`,
  `search_parity_test`, `server_lifecycle_test`, `mcp_tools_integration_test`,
  `spec_runner`, `spec_migration_test`) include the helper or call
  `setup_real_project`.
- `tests/helpers/real_project_index_cache.h` **does not exist** and is not
  included anywhere. It was a planned artifact; Step 0 shows it must not be
  created (see "Why no cache is written").

So this slice's scope reaches **only the 26.9s real-project path**, not the
load-bearing 272.9s integration cost.

## Step 0 — Is repeated per-corpus indexing measurably present in reach?

Measured on this checkout, warm ccache, `build/release`:

| measurement | command | wall (gtest total) |
| --- | --- | --- |
| full suite, fresh process #1 | `lci_real_project_tests` | 28.68s (24.57s) |
| full suite, fresh process #2 | `lci_real_project_tests` (again) | 31.67s (27.55s) |
| index **all 10 corpora once** | `--gtest_filter=RealProjectAvailabilityTest.EachAvailableProjectIndexes` | 28.65s (24.81s) |
| single chi test (chi = 1 MB) | `--gtest_filter=RealProjectIndexingTest.GoChiIndexesSuccessfully` | 0.15s |
| 43 chi-touching tests | `--gtest_filter=*Chi*:*chi*` | 10.07s |

Two load-bearing facts fall out:

1. **The one-time cold index of all 10 corpora (24.81s) ≈ the entire 75-test
   suite (24.57s).** The other 74 tests reindex **nothing** — they are near-free
   query work against the cached indexes. The existing in-process cache in
   `setup_real_project` (a `static std::map<key, CacheEntry>` keyed by
   `path|name`) has **already eliminated all within-process repeated indexing.**
   There is no repeated per-corpus indexing left in reach to reclaim.

2. **Two fresh processes each pay the full ~25s** with zero benefit from the
   first. Confirms there is **no cross-process persistence** anywhere. The cost
   is dominated by the large corpora: fastapi (57 MB), pocketbase (23 MB),
   trpc (15 MB), okhttp (11 MB).

The only win still on the table is **cross-process persistence** — turning the
24.8s cold-index floor into a warm on-disk load on the 2nd+ gate run of an
iterative loop session.

## Why no cache is written (prove-before-build outcome)

A valid content-addressed cache must, on a hit, **skip `MasterIndex::index_directory`
and reload an index that produces results identical to a cold build.** That
requires serializing the built index (symbol store + trigram index + postings +
reference tracker + side-effect data) to disk and reloading it.

- **`MasterIndex` has no serialize / deserialize / save / load-from-disk API.**
  Every "serialize" hit in `src/indexing/` is *mutex* serialization, not disk
  persistence. There is no on-disk index format anywhere in `src/` or `include/`.
- Adding one means editing `src/indexing/master_index.{h,cpp}` (and the symbol
  store / trigram / postings / reference-tracker types) — **all outside this
  slice's file scope.**
- Without a real serialized index, the only thing a `real_project_index_cache.h`
  could produce is a directory-exists / re-index-anyway shim. That is explicitly
  forbidden: "Never accept a cache just because a directory exists"
  (`.claude/rules/karpathy-principles.md` #6, fail-fast — no
  implemented-but-empty stubs). It would also fail the required discrimination
  test by construction.

Therefore the correct, disciplined result is **no cache code**, and
`real_project_index_cache.h` is **not created**. Building it would be over-build
against a proof that says it cannot exist within scope.

## Scope-expansion proposal (load-bearing for S4's <180s target)

**S3's scope cannot deliver the <180s target.** Arithmetic:

- Full gate median **376.2s**.
- Best case if a cross-process cache reclaimed the *entire* real-project cold
  floor: 376.2 − 24.8 ≈ **351s**. Still ~2× over 180s.
- The **272.9s `lci_integration_suite` (67.5%) is the only cost whose removal
  reaches the target**, and it is out of this slice's file scope.

Two follow-ups, both outside `tests/helpers/`:

1. **Attack the 272.9s integration suite (the real S4 lever).** It reindexes
   parity corpora by spawning the `lci` daemon per corpus across many spec
   descriptors (`tests/integration/spec_runner.cpp` + the `lci` CLI). Options to
   evaluate: reuse one warm daemon per corpus across descriptors, or persist the
   daemon's index. Scope: `tests/integration/spec_runner.cpp`, `src/server/`,
   `src/cli/`, `src/indexing/` — **not** `tests/helpers/`.

2. **Persist the in-process real-project index across gate runs (the 24.8s
   secondary lever).** Requires a real `MasterIndex` on-disk serialize/deserialize
   in `src/indexing/master_index.{h,cpp}`, keyed by a content-addressed key
   (corpus content hash + LCI index/schema version + relevant index config +
   cache-format version), atomic publish (temp + rename), fail-fast rebuild on
   any miss/corruption. Only *then* can `tests/helpers/` reuse it and the
   discrimination test become meaningful. Saves ~24.8s per repeat gate run of a
   loop session; saves 0 on a one-shot cold gate.

Recommendation: do **not** expand this slice's file scope to chase either.
Route them as separate tasks with the scopes above. This slice closes as a
measured no-change with the reachability finding recorded.

## Reproduce

```
cmake --build build/release --parallel --target lci_real_project_tests
# full suite, two fresh processes (no cross-process persistence):
./build/release/tests/lci_real_project_tests
./build/release/tests/lci_real_project_tests
# one-time cold index of all corpora ≈ whole suite (in-process cache proof):
./build/release/tests/lci_real_project_tests \
  --gtest_filter=RealProjectAvailabilityTest.EachAvailableProjectIndexes
```

Gate (once, at close): `ctest --test-dir build/release --output-on-failure -j4`.
