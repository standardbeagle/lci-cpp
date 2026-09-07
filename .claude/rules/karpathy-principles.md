# Karpathy Principles — LCI C++ Port

Performance discipline for the LCI C++ port. Speed is a feature. The Go binary is the bar; C++ must match or beat it on every hot path. Slow code is a defect.

## Core Mandate

**No lazy slowdowns. No cutting corners.**

LCI's reason to exist is sub-millisecond semantic code search with 79.8% context reduction vs Grep. Every regression on parity or latency negates the project's value. Treat perf as correctness — a green test that ran 10× too slow is a red test.

## Hard Rules

### 1. Measure before claiming done
- Wall-clock the hot path. Compare to Go on the same corpus, same query.
- "Faster than I expected" is not a measurement. Numbers in the PR/commit/Dart comment, or it didn't happen.
- Stability: 10/10 stable runs minimum on parity tests before marking Done.
- Floor check: full non-parity unit suite must hold or improve vs prior iteration baseline. Regressions block merge.
- An **inherited** baseline — one measured by an earlier attempt, a dead agent's transcript, or a
  sibling slice — is legitimate evidence, but only when it is DISCLOSED as inherited and the
  conditions are stated: same corpus, same pre-change tree, run count and spread, and a delta at
  least an order of magnitude above that spread. Re-measuring is the default; reusing is the
  exception you write down. Silently presenting an inherited number as freshly measured denies the
  reviewer the one check that makes it sound. Here: 3 pre-change runs spanning 0.4% (172.4-173.1 MB)
  carried a +110 MB delta, disclosed in the comment and validated by the reviewer.
  <!-- written_at: 2026-09-04T15:00:00Z  source_event: task:01M1NCSJ31MG00VGMWRSRS88J1, comment:01M1PBKWV6TNCGS700RY105RKD, comment:01M1PD8VGXBQWQDC2YP4JKAPV6 -->
- **A correct fix can falsify a justification written in another file.** The caller-audit clause
  (rule 3) covers a caller that cancels your fix; this is its mirror — your fix silently invalidates
  a documented premise somewhere outside your `fileScope`, and nothing fails. Any change to what a
  resource IS (file-backed vs anonymous, borrowed vs owned, cached vs recomputed) obliges a grep for
  the prose that reasons about that property, and a report on every site the change makes false.
  Here `a9bf1ac` moved content from mmap to heap, falsifying `src/server/server.cpp:97-101`
  ("since the content store retains file-backed mmaps, VmRSS counts page-cache pages the kernel
  reclaims on its own") and the sibling rationale at `include/lci/config.h:75` — the RssAnon self-cap
  now bounds bytes it was written to exclude (filed: `01M1PE7698726F1P9QQMTQ5FT1`).
  <!-- written_at: 2026-09-04T15:00:00Z  source_event: task:01M1NCSJ31MG00VGMWRSRS88J1, comment:01M1PE4T1Y319BETECJGPYKCW5, git:a9bf1ac -->

### 2. No allocation in inner loops
- Pre-size containers. Reserve `std::vector` / `std::string` capacity from known bounds.
- Reuse buffers across iterations. Thread-local scratch where lifetimes permit.
- Move, don't copy. `std::move` on terminal use; `const&` on pass-through.
- No `std::string` returned by value from per-symbol / per-token / per-line functions — return `std::string_view` against an owning buffer, or write into a caller-provided sink.
- No `shared_ptr` in hot paths. Ownership is intent, not insurance — pick `unique_ptr`, raw pointer, or value, and document lifetime.

### 3. No mutex on the read path
- LCI is read-heavy. Hot reads use lock-free structures or RCU-style snapshots.
- Mutex is acceptable on the indexing write path; never on `/search`, `/browse-file`, `/list-symbols`, `/references`, `/tree`, `/inspect-symbol`.
- If you must lock on a read, prove it (benchmark, contention numbers) and document the trade in the file header.
- A lock-free read path is only as good as its worst-published snapshot. `clear()` on any
  RCU-backed store publishes an EMPTY generation the instant it runs — every concurrent
  reader sees "not found", and search silently degrades to scan-all. Clear inside the bulk
  window / on the commit path, never before it opens.
  <!-- written_at: 2026-09-04T14:00:00Z  source_event: task:01M1NCSJ31P21DT2VVB2H0DSKS, git:15f8f52, git:8acde22 -->
- **Audit every caller of an API you change, including the read-only ones your `fileScope`
  excludes.** A caller that pre-clears, pre-locks, or pre-resets before calling the fixed API
  cancels the fix for that path, and the fix ships looking complete. Two callers of the same
  API that differ in their preamble is the tell — diff them before claiming the root cause is
  closed. Here `server.cpp:455` got the full benefit while `handle_reindex`
  (`src/server/server_endpoints.cpp:341`) called `indexer_->clear()` immediately before
  `index_directory()`, so the HTTP `/reindex` path never saw the fix (filed:
  `01M1PA4DNHVBWX1749E3KQJA5N`). Out-of-scope means do-not-edit, never do-not-read.
  <!-- written_at: 2026-09-04T14:00:00Z  source_event: task:01M1NCSJ31P21DT2VVB2H0DSKS, comment:01M1PA20HWVVR13XS0BHX3ETHM -->
- **Mutual exclusion with the bulk window is STRUCTURAL — a liveness CHECK before a write is a
  TOCTOU, and a documented INVARIANT names the mechanism you must implement.**
  `master_index.cpp:309-319` already said the bulk clear->publish span must exclude the
  `snapshot_mu_` writers if watch mode were ever wired. The first wiring answered it with
  `MasterIndex::is_indexing()` on ONE of three write arms (`WatchPipeline::on_rebuild`), leaving
  `on_event`'s Remove and new-path arms unguarded: `/reindex` can open its window between the check
  and `update_file`, the incremental write lands in the staging generations, and the commit path's
  `clear()` either wipes it or publishes the file's symbols and postings twice. The fix is one lock
  line per writer — `index_file`/`update_file`/`remove_file` take `bulk_mu_` (blocking) BEFORE
  `snapshot_mu_` (`734ced3`). Lock order is `bulk_mu_ -> snapshot_mu_` everywhere; `clear()` takes
  `snapshot_mu_` only, so nothing inverts it, and read paths stay lock-free.
  **Corollary: the thread that receives OS events must never be the one that blocks.** Once the
  writers can wait out a whole reindex, an inline `remove_file`/`index_file` on the efsw callback
  thread overflows the inotify queue. Route every such event through the debouncer/timer thread —
  the callback only inserts into a deduplicating pending-path set and kicks the timer (`9fe0243`,
  path-only events keyed under a `FileID{0}` sentinel). Any NEW `MasterIndex` write caller (MCP
  edit tools, CLI incremental index) inherits both halves.
  <!-- written_at: 2026-09-07T16:10:00Z  source_event: task:01M1NCSJ31A8XHPC7NKJEGS7RV, comment:01M1XARQG97QQRBYFEC4M06YBJ, comment:01M1XAS4V93FV6DWZQX0HTQM2W, git:734ced3, git:9fe0243 -->
- **A TSan-clean run over tests that never overlap the two paths proves nothing; the RED test must
  PARK the other thread inside the window via a test hook.** Attempt 1 reported 96/96 TSan-clean
  and still shipped the TOCTOU, because no test ran `index_directory` and `update_file`
  concurrently. `MasterIndexTest.UpdateDuringBulkWindowSurvivesExactlyOnce` (`d7362e4`) parks the
  bulk thread in `set_post_parse_hook`, signals, runs `update_file`, and joins — deterministic
  3/3 FAIL on a detached worktree at the pre-fix sha, stable pass after. Sleep-and-hope makes the
  overlap a scheduler accident; a bounded give-up in the hook keeps the FIXED code (which now
  blocks on `bulk_mu_`) from deadlocking the test. Shape for any "these two paths are mutually
  exclusive" claim: hook the slow path at its window boundary, signal from the hook, race the
  call, join.
  <!-- written_at: 2026-09-07T16:10:00Z  source_event: task:01M1NCSJ31A8XHPC7NKJEGS7RV, comment:01M1XCPF5M9AKWQY8TMAEJD37H, comment:01M1Y9DE31CFD1CETYMFJKRX7S, git:d7362e4 -->

### 4. Determinism is non-negotiable
- File IDs, symbol IDs, scan order, output ordering — deterministic across runs and across machines for the same corpus.
- No reliance on hash-iteration order in user-visible output. Sort before emit.
- Reference iter-3 (`a1964b2`): three latent concurrency bugs were perf+correctness wins simultaneously. Race-free is faster than race-with-retry.
- **Parallel arrays derived from one byte stream are produced by ONE step function, never two
  hand-written walkers.** Two walkers over the same grammar drift on the inputs nobody wrote a
  test for — and the drift is silent: indices that address each other stop lining up, and a
  prefilter certifies present data absent. `src/core/trigram.cpp` held exactly that pair:
  `to_code_points_into` skipped an invalid UTF-8 lead byte while `compute_byte_offsets_into`
  did not, so `code_points[i]`/`byte_offsets[i]` desynced and one Latin-1 `0xC0-0xEF` byte
  swallowed the following ASCII — `TrigramBloom::build` omitted real trigrams and
  `Narrowing::certifies_absent` certified a present substring absent. Fixed by giving both
  walkers one shared `utf8_seq_step` (`38ffb4e`, RED `99a2628`).
  The acceptance shape: a test pinning EQUAL LENGTHS from both producers, with one case per
  invalid-input class — bad lead byte, truncated sequence, bad continuation, overlong encoding.
  Same certified-absence class as `bench-harness-oracle-independence.md` §5 (mirror-written
  soundness check) and the `search-certified-absence-narrowing` memory; that rule covers two
  checks over one grammar, this one covers two producers over one stream.
  <!-- written_at: 2026-09-07T04:00:00Z  source_event: task:01M1NCSJ31ZEV9XGETA6VAF8CK, git:99a2628, git:38ffb4e, comment:01M1WX8DTZ08X1ZS4HYP05FKB1 -->

### 5. No mocking the database
- Integration tests hit the real indexer + real corpus.
- Parity tests run both Go and C++ binaries against the same corpus.
- Unit tests for pure logic only. Anything touching I/O, tree-sitter, or the symbol store runs against real data.

### 6. Fail fast, surface signal
- No silent fallbacks. No "implemented but returns empty" stubs (the `handle_git_analyze` pattern — return a clear error, not zeroed output).
- Errors at boundaries: parse failure, missing file, unsupported language → propagate. Do not paper over.
- A skipped corner is a bug filed in Dart with a `loop-fix` tag and the exact missing surface named. Never quietly ignored.

### 7. No "we'll optimize later"
- Optimize on the path you're already touching. Future-you will not remember why this hot path is slow.
- "Premature optimization" is not a license to write `std::string + std::string` in a tokenizer. Big-O choice is design, not optimization — get it right the first time.
- Algorithm change → benchmark before and after, post numbers.

## Anti-Patterns (auto-reject in review)

| Pattern | Why bad | Fix |
|---|---|---|
| `std::string` return from per-token function | malloc per token | `string_view` + owning buffer |
| `std::map<std::string, X>` keyed by symbol path | log-N + cache miss per lookup | `flat_hash_map` / `unordered_map` with reserve |
| `auto x = vec;` (copy where move suffices) | full vector dup | `auto x = std::move(vec);` |
| `shared_ptr<T>` on read-only data | refcount traffic + cache thrash | raw pointer or `T*` into stable storage |
| `try { ... } catch(...)` swallowing errors | hides perf cliffs and correctness bugs | catch specific type, log+rethrow or fail |
| `std::regex` in hot path | order-of-magnitude slower than RE2/handwritten | RE2 or explicit state machine |
| New thread per request | thread-create cost > work | thread pool, fixed size, bounded queue |
| Recompute on every call | obvious cache miss | memoize with explicit invalidation |
| `std::endl` in tight loops | forces flush | `'\n'` |

## Parity Discipline

- Every parity descriptor change requires a `_rationale` field on every tier (`stable`, `ignore`). No silent ignore expansion.
- Decision documents (Decision A, B, C in `MODULE_MAP.md`) record *why* a divergence is accepted, not just *what* differs.
- Phantom-failure pattern (iter-6, iter-9): always run target test 10/10 before assuming the task description is current. Stale tasks waste iterations.

## When in doubt

Ask: "Would a C engineer in 2005, looking at this, call it slow?" If yes, rewrite. The C++ port exists to be fast — every concession to convenience is a step toward "just use the Go binary."

Slow C++ has no reason to exist.
