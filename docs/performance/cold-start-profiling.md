# Cold-start profiling — large corpora (2026-08-30)

perf sampled (300Hz, frame pointers, `profile` preset) over the full
`lci mcp` cold start on the dotnet corpus (15,797 files / 597k symbols /
1.36M refs, the densest local corpus). WSL2 note: the perf wrapper's
kernel symlink must be refreshed after linux-tools upgrades
(`/usr/lib/linux-tools/$(uname -r)/perf` → the versioned tools dir).

## Where the 151s went (baseline HEAD=628a7b7)

| Phase | CPU share | Wall (est.) | Notes |
|---|---|---|---|
| parallel index (FileProcessor workers) | 55.7% | ~54s | ts parse 53%, extract 17%, malloc family ~17% of total |
| MCP warmup: populate_side_effects_from_ast | 31.2% | ~85–90s | SERIAL whole-corpus re-read + re-parse |
| warmup: heuristic + propagate + engine | ~1% | ~2–5s | |
| scan/misc | ~5% | few s | |

## Fix 1 (landed): record side effects inside the index pipeline

The warmup's AST pass duplicated, on one thread, the exact
`UnifiedExtractor` walk the parallel index performs anyway. Theoretical
best case: ZERO extra parse — attach the side-effect sink at extraction
time. Implemented as per-worker `SideEffectAnalyzer` instances drained
into the runtime's analyzer per file under a short lock
(`FileProcessor::set_side_effect_target`, plumbed
MasterIndex → Pipeline → FileProcessor; wired by run_mcp and
run_server before their index builds).

Measured (same corpus shape, id-identical index stats):

| Corpus | Before | After (best of 3) | Peak RSS |
|---|---|---|---|
| dotnet | 151s / 3.81GB | **47.3s** (47.3/57.0/48.2) / **1.93GB** | −69% wall, −49% RSS |
| next.js | 23.8s / 547MB | **14.4s** / 508MB | −40% wall |

Warmup overhead beyond indexing: ~96s → ~2.3s. The RSS drop is the
serial re-parse's transient tree/content allocations no longer stacking
on top of the retained index.

Residual (unchanged semantics): watcher/incremental reindex still does
not update side-effect records (the old one-shot pass had the same
staleness, now bounded per file when a bulk reindex runs);
functions deleted from a file leave stale keys until the next bulk
index.

## Fix 2 (landed): scanner readlink storm + glob/gitignore prefilters

Per-stage wall timing (now printed by every bulk index:
`lci: index stages: scan=… parse=… integrate=…`) attributed the
remaining time: scan=12.8s, parse=23.6s, integrate=5.1s on dotnet.
perf over `lci list` split the scan: 39% `fs::relative` →
`weakly_canonical` → one readlink syscall per path component per file;
25% `match_glob_at` backtracking; 18% gitignore Wildcard patterns
re-running the glob at every path suffix.

- Repo-relative paths are now derived lexically through the walk
  (append entry name to the parent's rel prefix) — zero readlinks; the
  theoretical best for "compute a relative path you already know".
- Scanner include/exclude globs and gitignore Wildcard patterns carry
  their longest wildcard-free literal (slash-trimmed — `**/` can
  collapse); a `find()` miss skips the backtracking matcher and the
  per-suffix retry loop entirely.

Measured on dotnet (identical corpus: 15,797 files / 596,617 symbols /
1,363,805 refs): scan 12.8s → 2.5s, wall 47.3s → **35.8s**.

## Remaining gap to theoretical best (next targets, with profile data)

Current dotnet split: scan 2.5s + parse 21.9s + integrate 3.5s +
runtime warmup/engine ~8s = 35.8s wall.

1. **Allocator contention — CONFIRMED, ~8s of parse wall, next slice.**
   The worker-phase profile shows malloc family ~25% of CPU with
   visible futex wait (glibc arenas under 12 threads; most traffic is
   tree-sitter internal ts_malloc). A/B via
   `LD_PRELOAD=libtcmalloc_minimal.so.4`: parse 21.9s → 14.0s, wall
   35.8s → **27.2s** (−24%), RSS unchanged (1.91GB). Shipping it is
   its own slice: static-link tcmalloc_minimal (the DYNAMIC gperftools
   auto-link already broke a release tarball once — see
   self-contained-release-binary-traps), verify all three CI legs and
   the RSS self-cap behavior. Until then server deployments can take
   the win today with the LD_PRELOAD line above.
   SHIPPED (f4db445): static link, Linux default ON, sanitizer/
   gperftools guards, tests link it, CI installs the archive.
   Interposition verified in the binary (weak `malloc`/`free` + 40 tc_*
   symbols, no new shared deps). A quiet-machine wall-clock confirmation
   of the shipped binary is still owed — every post-link benchmark ran
   against a 12-30 load average from unrelated agent fleets; re-run
   best-of-3 on an idle box before quoting a final number
   (the LD_PRELOAD 27.2s A/B is the same library, quiet machine).
   PAIRED confirmation of the shipped binary (interleaved glibc-variant
   vs shipped, same contended box): 73.9→50.4s and 76.5→39.3s
   (−32%/−49%; a third pair inverted when load spiked to 32 mid-run —
   contention noise, not signal). The win grows under load: glibc's
   own M_ARENA_MAX=4 cap (set in main.cpp for RSS) concentrates the
   contention tcmalloc's per-thread caches avoid.
2. **malloc family ~17% of CPU** (ts_malloc + malloc + _int_malloc) —
   allocation traffic in parse+extract. tree-sitter's internal
   allocations dominate; an arena for extractor-side vectors is the
   in-our-control slice (karpathy rule 2).
3. **`ts_language_table_entry` 7.8%** — C# grammar table lookups inside
   tree-sitter; not ours to fix (grammar-dependent).

Theoretical best cold start on dotnet-class corpora ≈ scan (~1-2s) +
parse floor (~8s) + integrate (~3s) + warmup heuristics (~3s) ≈
**12–15s**. With the tcmalloc slice landed that is ~27s vs the floor —
the last ~2x is extraction-side allocation diet (visit_node/get_results
string copies), the ~8s runtime-warmup/engine tail, and tree-sitter's
own grammar-table cost (not ours). Progression this campaign:
151s → 47.3s (side-effect fold-in) → 35.8s (scan fixes) → 27.2s
(allocator, pending packaging).
