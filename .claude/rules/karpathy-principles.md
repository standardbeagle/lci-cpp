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

### 4. Determinism is non-negotiable
- File IDs, symbol IDs, scan order, output ordering — deterministic across runs and across machines for the same corpus.
- No reliance on hash-iteration order in user-visible output. Sort before emit.
- Reference iter-3 (`a1964b2`): three latent concurrency bugs were perf+correctness wins simultaneously. Race-free is faster than race-with-retry.

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
