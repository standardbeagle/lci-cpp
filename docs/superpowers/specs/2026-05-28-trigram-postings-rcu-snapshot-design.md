# Trigram + Postings Lock-Free RCU Snapshot — Design Spec

**Date:** 2026-05-28
**Status:** Approved (brainstorm), ready for implementation
**Epic (worktrack/lci):** `01KSRKRW8VZB3AEJ97GGNJDMJW`
**Unblocks:** `01KSR5KGDKQG41FQV4DJY76302` (drop 4 read locks from `execute_search`)
**Parent EPIC:** `01KSR5H2ZC5C6AEQH83N3S11KE` (LCI C++ port review remediation)

## Problem

`MasterIndex::execute_search` (the `/search`, `/refs`, definition-lookup path) acquires
four `std::shared_timed_mutex` ReadGuards per query (Trigram, Postings, Reference,
Content) via `IndexLockManager::try_lock_shared_for`. The remediation task asked to drop
these as "redundant insurance over a lock-free RCU snapshot."

That premise is **false** and was refuted empirically:

- The RCU snapshot (`MasterIndex::snapshot_`, `atomic<shared_ptr<const FileSnapshot>>`)
  covers only the path↔FileID `file_map`. It does **not** cover the structures
  `execute_search` actually queries: `trigram_index_`, `postings_index_`, `ref_tracker_`.
- `TrigramIndex` stores a plain `absl::flat_hash_map<uint32_t, TrigramEntry> ascii_trigrams_`
  (+ `unicode_trigrams_`, `sharded_storage_`, a non-atomic mutable `search_cache_`),
  mutated in place by `index_file`. `PostingsIndex` stores plain
  `flat_hash_map<string, flat_hash_map<FileID,int>> tokens_` + `reverse_keys_`, likewise.
- **tsan proof (Debug `-fsanitize=thread`):** with the locks *present*,
  `MasterIndexSearchIntegrationTest.ConcurrentSearchDuringIndexing` is already race-red.
  Reader `TrigramIndex::find_candidates_with_options` (trigram.cpp:397) races writer
  `TrigramIndex::index_file` (trigram.cpp:323) resizing `ascii_trigrams_`. The
  `shared_timed_mutex` ReadGuards do **not** make the search path race-free today.

Enumerated tsan races (12 warnings across `ConcurrentSearchDuringIndexing` +
`ConcurrentSearchReads`) are confined to exactly two classes:

| Index | Reader frame | Writer frame |
|---|---|---|
| Trigram | `find_candidates_with_options` (trigram.cpp:397) | `index_file` (trigram.cpp:323) |
| Postings | `find` (postings.cpp:168) | `index_file` / `index_file_pretokenized` / `remove_file` (130/121/142) |

No `ReferenceTracker`, `FileContentStore`, or snapshot-path-string races appear.
Therefore the Reference and Content ReadGuards in `execute_search` *are* genuinely
redundant (those structures are already lock-free); only Trigram + Postings need work.

## Goal

Make `TrigramIndex` and `PostingsIndex` safe for concurrent reads during writes via
RCU/atomic-`shared_ptr` snapshots — mirroring the proven `FileContentStore` pattern — so
that the dependent task can drop all four read locks and reads become genuinely lock-free
(karpathy Rule 3).

## Non-goals

- Changing `ReferenceTracker` or `FileContentStore` (already safe).
- Removing the locks from `execute_search` — that is the dependent task (b).
- Altering search semantics, ranking, or output ordering (must stay byte-identical).

## Design

### Pattern (mirror `FileContentStore`)

Each index holds:

```cpp
std::atomic<std::shared_ptr<const Snapshot>> snapshot_;  // lock-free reads
mutable std::mutex write_mu_;                            // serializes writers
```

- **Read path:** one `snapshot_.load(acquire)` → operate on the immutable `Snapshot`.
  Zero locks. The `shared_ptr` keeps the snapshot alive for the duration of the read even
  if a writer swaps mid-read.
- **Write path:** `lock_guard(write_mu_)` → clone the current snapshot → mutate the clone
  → `snapshot_.store(new, release)`. `write_mu_` exists only to stop two writers cloning
  the same base and losing each other's edits (same rationale as `FileContentStore::write_mu_`).

### `TrigramIndex::Snapshot`

Immutable struct holding the current read state:

```cpp
struct Snapshot {
    absl::flat_hash_map<uint32_t, TrigramEntry> ascii_trigrams;
    absl::flat_hash_map<std::string, TrigramEntry> unicode_trigrams;
    ShardedTrigramStorage sharded_storage;
    absl::flat_hash_set<FileID> invalidated_files;
};
```

`search_cache_` is removed from the read path — see "search_cache_ disposition".

### `PostingsIndex::Snapshot`

```cpp
struct Snapshot {
    absl::flat_hash_map<std::string, absl::flat_hash_map<FileID, int>> tokens;
    absl::flat_hash_map<FileID, std::vector<std::string>> reverse_keys;
};
```

### Two write modes (critical — guards index throughput)

`index_directory` drives thousands of `index_file` / merge calls. A whole-snapshot COW
*per file* would be **O(files²)** and destroy indexing throughput. So writes split:

1. **Bulk build** — `index_directory` via `FileIntegrator` + `merge_bucketed_trigrams` /
   `merge_postings`. Writes accumulate into a **private, unpublished** mutable snapshot;
   a **single** `atomic store` publishes it when the pipeline finishes. No per-file COW.
   The existing `bulk_indexing` atomic flag already marks this window — reuse it to select
   the staged path. Concretely: provide a `begin_bulk()` returning a builder (or set a
   staging member) that the integrator writes into, and `publish_bulk()` that swaps once.
2. **Incremental** — single-file `index_file` / `update_file` / `remove_file`. Each does a
   clone-mutate-swap under `write_mu_`. O(snapshot) per call, acceptable because only the
   file-watcher / test path hits it (the server's reindex uses full `clear()` +
   `index_directory`, i.e. the bulk path).

### `search_cache_` disposition

`find_candidates_with_options` reads and writes the `mutable search_cache_` under only a
*shared* read lock — a second data race (two concurrent readers both mutate it).

Decision rule (executed during implementation, numbers recorded on the task):

1. Benchmark search latency with the cache **disabled** vs current, on the same corpus
   (`BM_Search*`, `BM_RealProjectSearch*`).
2. Trigram candidate lookup is already sub-µs; if the delta is within noise → **delete**
   the cache (removes the race, no read-path mutation, simplest — preferred).
3. Only if the benchmark shows a material win → keep it as a **read-only** member of the
   immutable `Snapshot` (populated at build time, never mutated on read).

## Error handling

No new failure modes. `shared_ptr` allocation failure on snapshot clone throws
`std::bad_alloc` and propagates (exception-neutral; the write is abandoned, the prior
snapshot stays published — readers unaffected). No silent fallback.

## Testing strategy

- The existing concurrent tests are the spec: `ConcurrentSearchDuringIndexing` and
  `ConcurrentSearchReads` go from tsan-**red** to tsan-**clean**. No mocks; real corpus.
- Full `ctest` (release) must stay green.
- If a coverage gap exists, add a targeted "incremental `update_file` + `remove_file`
  under concurrent search" case (the watcher pattern).
- tsan gate runs against `lci_tests` (target), filter `*Concurrent*`.

## Acceptance gates (`cpp-perf` workflow)

- `ConcurrentSearchDuringIndexing` + `ConcurrentSearchReads` tsan-clean (currently red).
- Full `ctest` green.
- Search µbench ≥ baseline (locks present, build/release):
  `BM_SearchSmallIndex` 16269 ns · `BM_SearchMediumIndex` 39831 ns ·
  `BM_RealProjectSearchChi` 81.3 µs · `BM_RealProjectSearchFastapi` 213 µs ·
  `BM_RealProjectSearchLatency` 54.1 µs.
- **Index-throughput µbench must not regress** (guards the bulk-COW risk):
  `BM_*Index*` / `index_performance_test`.
- Benchmark numbers posted on the task (karpathy: numbers or it didn't happen).

## Infra prerequisite (done)

`src/CMakeLists.txt` linked gperftools/tcmalloc into `lci_lib` unconditionally whenever
present on the host, which aborts every sanitizer build at startup
(`Attempt to free invalid pointer`). Added a `-fsanitize` guard so asan/tsan/msan presets
link and run. This unblocks the tsan gate for all perf work. Not on the search hot path.

## Implementation tasks (worktrack subtasks of the epic)

1. **IT1 — TrigramIndex → RCU snapshot.** Snapshot struct, `atomic<shared_ptr>`,
   `write_mu_`, reads via `load`, incremental COW writes, bulk-build single-publish.
2. **IT2 — PostingsIndex → RCU snapshot.** Same pattern; covers `find`, `index_file`,
   `index_file_pretokenized`, `remove_file`, `clear`, bulk publish via `merge_postings`.
3. **IT3 — search_cache_ benchmark + remove/embed.** Execute the decision rule above.
4. **IT4 — Integration gate.** tsan-clean both concurrent tests, full ctest, search +
   index benchmarks vs baseline, record numbers. Depends on IT1–IT3.

Task (b) `01KSR5KGDKQG41FQV4DJY76302` (drop the 4 locks) starts only after this epic is
Done + tsan-clean.
