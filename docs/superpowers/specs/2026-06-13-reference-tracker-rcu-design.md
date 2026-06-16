# Lock-free ReferenceTracker + Content view-pinning (def/refs read path)

**Task:** `01KSWHQ742` (P1). Enables dropping the remaining 2 of 4 `execute_search`
ReadGuards (Reference + Content). Follow-up to `01KSR5KGDK` (b), which dropped the
Trigram + Postings ReadGuards once those indexes became RCU.

**Date:** 2026-06-13. **Author:** loop (windows-mac-port).

---

## 1. Problem

`execute_search` (`master_index_search.cpp`) holds two remaining `IndexLockManager`
ReadGuards — Reference and Content. Both are **lifetime guards**, not lookup-atomicity:
the lookups hand back references into shared, in-place-mutated storage that the rest of
the function dereferences. Dropping them today is a use-after-free under concurrent
reindex.

### 1a. Reference (load-bearing — confirmed)

`ReferenceTracker::find_symbols_by_name` / `get_enhanced_symbol` /
`get_file_enhanced_symbols` / `get_symbol_at_line` return raw `const EnhancedSymbol*`
into `SymbolStore::data_`, which is a `std::vector<EnhancedSymbol>`. `process_file`
appends via `SymbolStore::set` → the vector reallocates → **every outstanding pointer
dangles**. `execute_search` (declaration_only / usage_only, lines ~115–211) sorts and
dereferences a vector of these pointers *after* the lookup. The Reference ReadGuard is
the only thing serializing this against `process_file` today.

### 1b. Content (load-bearing — confirmed)

`FileContentStore::get_content(id)` returns a `string_view` into a
`shared_ptr<FileContent>` that is held only by a snapshot the call loads locally; the
local `shared_ptr` dies at return. `execute_search` scans `content_sv` after the call
(line ~269+). A concurrent `invalidate_file` swap can free the `FileContent` →
dangling view. The Content ReadGuard covers this today.

### 1c. Read-path caches (PREMISE REFUTED — no work needed)

The task content (2026-05-29) claims `scope_chain_cache_` (`reference_tracker.cpp:537`)
and `reference_cache_` (`:637`) are "written during reads" → a read-vs-read race like
trigram's old `search_cache_`. **This is false.** Verified 2026-06-13:

- `scope_chain_cache_` is written only in `build_symbol_scope_chain`, called only from
  `process_file` (`reference_tracker.cpp:65`).
- `reference_cache_` is written only in `resolve_reference_target`, called only from
  `process_all_references` (`:136`).

Both are **write-path only**, already serialized by the Reference WriteGuard. No read
query mutates them. This sub-task evaporates — the caches stay where they are.

---

## 2. Design

### 2a. Content — trivial, no new API

`FileContentStore` already exposes the pin: `get_file(id)` returns
`std::shared_ptr<FileContent>` (header line 133). `execute_search` replaces its
`get_content(fid)` call with `get_file(fid)`, holds the `shared_ptr<FileContent>` as a
local for the duration of the match scan, and views it via `FileContent::view()`. While
the local `shared_ptr` is alive, a concurrent `invalidate_file` swap retires the entry
from the published snapshot but cannot free the `FileContent` — refcount keeps it alive
until the scan finishes. The Content ReadGuard is then droppable. **No FileContentStore
changes.**

### 2b. Reference — internal RCU, mirror FileContentStore

Convert `ReferenceTracker`'s read-side state to an immutable, atomically-swapped
snapshot. The state that read queries touch and that `process_file` /
`process_all_references` / `remove_file` mutate:

```
struct Snapshot {
    SymbolStore symbols;                                       // data_ + indices
    absl::flat_hash_map<uint64_t, Reference> references;
    absl::flat_hash_map<SymbolID, std::vector<uint64_t>> incoming_refs;
    absl::flat_hash_map<SymbolID, std::vector<uint64_t>> outgoing_refs;
    absl::flat_hash_map<FileID, std::vector<ScopeInfo>> scopes_by_file;
    absl::flat_hash_map<SymbolID, std::vector<ScopeInfo>> symbol_scopes;
    absl::flat_hash_map<FileID, absl::flat_hash_map<int, std::vector<int>>>
        line_to_symbols_by_file;
    ReferenceStats stats;
};
AtomicSharedPtr<const Snapshot> snapshot_;
mutable std::mutex write_mu_;          // serializes writers (clone base + publish)
std::shared_ptr<Snapshot> staging_;    // non-null inside a bulk-index window
```

**Stays mutable / outside the snapshot** (write-path only, never read by queries):
`reference_cache_`, `scope_chain_cache_`, `import_resolver_`, `import_data_`,
`next_symbol_id_`, `next_ref_id_`. These are touched only under the Reference WriteGuard
during indexing.

**Write path:** mirror Trigram/Postings — `lock_guard(write_mu_)` → clone the current
snapshot → mutate the clone → `snapshot_.store(new, release)`. Bulk-index window
(`bulk_indexing`) mutates `staging_` in place and publishes once on close, avoiding
O(files²) per-file clones (same rationale as the trigram epic).

**Read path / pin API:**
```
std::shared_ptr<const Snapshot> pin() const { return snapshot_.load(); }
```
Pointer-returning read methods (`find_symbols_by_name`, …) load the snapshot, operate on
its `symbols`, and return pointers into it. To keep those pointers valid past the call,
the **caller holds the pin**. Two call-site classes:

- `execute_search`: pins once at function entry (`auto refs_snap = ref_tracker_.pin();`),
  drops the Reference ReadGuard, uses the snapshot for the whole body.
- The other 11 files / 53 sites: **unchanged** in phase 2–3. They keep acquiring the
  Reference ReadGuard, and `process_*` keeps acquiring the Reference WriteGuard. The
  WriteGuard blocks publish+free while an unpinned reader holds the ReadGuard, so their
  raw pointers into the then-published snapshot stay valid. Internal RCU does not break
  them; it only adds the pin path that `execute_search` uses to opt out of the lock.

This is the contained, correct interim: lock-free def/refs **for the search hot path**;
the lock remains for the colder MCP-handler read sites. Retiring the Reference index
entirely (migrating all 53 sites to pin) is a separate follow-up.

---

## 3. Phases (each independently green + committable)

- **Phase 0 — coverage (red first).** Add `ConcurrentDefRefsDuringIndexing` to
  `master_index_search_test.cpp`: index a corpus with a stable symbol, fire N reader
  threads calling `search_definitions` / `search_references` for it while a writer
  churns an unrelated file. Prove it tsan-RED by temporarily dropping the Reference +
  Content ReadGuards (local diff, not committed) → confirm the race reproduces → restore.
  Commit the test (passes with locks present; it is the gate for phases 1 & 3).
- **Phase 1 — Content pin.** `execute_search` uses `get_file` + holds
  `shared_ptr<FileContent>`; drop the Content ReadGuard. Gate: phase-0 test + full
  ctest + tsan `*Concurrent*` + ubench.
- **Phase 2 — ReferenceTracker internal RCU.** Snapshot struct + `write_mu_` +
  clone-mutate-publish + bulk window + `pin()`. No caller changes; Reference
  ReadGuard/WriteGuard stay. Gate: full ctest + tsan + ubench ≥ baseline (clone cost on
  the single-file write path must not regress incremental indexing — measure).
- **Phase 3 — drop Reference ReadGuard in execute_search.** Pin the ref snapshot for the
  function body; drop the guard. Gate: phase-0 test now exercises the lock-free path
  10/10 tsan-clean.
- **Phase 4 (follow-up task, not this one).** Migrate the 11 handler files to pin, retire
  the Reference entry from IndexLockManager.

This task `01KSWHQ742` covers phases 0–3 — **DONE 2026-06-13** (commits 03b5a01,
a3ee844, 043556c; execute_search fully lock-free; all 13 *Concurrent* tsan-clean;
ctest 1842/1842). Phase 4 is filed separately.

### Phase 4 follow-up (not this task)
Migrate the other ~11 files / 53 raw-pointer read-call sites
(`get_enhanced_symbol`/`get_file_enhanced_symbols`/`get_symbol_at_line`/… in
mcp handlers, engine, server, git, semantic/side-effect analyzers) to pin a
snapshot for the pointer lifetime, then drop their Reference ReadGuards and
retire the Reference entry from IndexLockManager. Today those callers are still
correct because they hold the Reference ReadGuard while a writer needs the
Reference WriteGuard (mutual exclusion keeps the published snapshot alive for
the synchronous handler). Reviewer advisory: that lock invariant is undocumented
at the call sites — document or migrate.

---

## 4. Risks

- **Clone cost on incremental writes.** Cloning the full Snapshot per single-file
  `update_file` is O(symbols). The bulk-index window covers `index_directory`; single-file
  updates pay one clone each — acceptable per the trigram epic's precedent, but
  **benchmark `update_file` before/after** and confirm no regression on the incremental
  path.
- **SymbolStore copyability.** RESOLVED — `SymbolStore` declares no deleted copy/move;
  members are `vector<EnhancedSymbol>` + `flat_hash_map`s; `EnhancedSymbol` is all value
  types (no raw pointers). Implicit value copy is correct — maps store ids/indices, not
  pointers, so nothing needs rebuilding.
- **Determinism.** Snapshot publish order is single-writer under `write_mu_`; output
  ordering already sorted in `execute_search`. No new nondeterminism.
- **`symbol_store_mut()`** RESOLVED scope — exactly one caller: `pipeline_integrator.cpp:133`,
  on the indexing (write) path. Phase 2 routes it through clone-mutate-publish (or folds the
  enrichment into the write closure); no read-path exposure.
