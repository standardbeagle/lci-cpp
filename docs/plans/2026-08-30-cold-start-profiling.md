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

## Remaining gap to theoretical best (next targets, with profile data)

The cold start is now ≈ the parallel index itself (~45s on dotnet).
From the profile:

1. **tree-sitter parse is 53% of CPU.** ~358G cycles across workers ≈
   7.5–9s of perfectly-parallel wall on this 12-thread machine — the
   parse floor. Observed index wall ~45s means parallel efficiency is
   the gap, not parse cost: suspects are the bounded-queue handoff,
   the single-threaded integrator stage consuming results, and worker
   starvation at the scan tail. Next step: per-stage wall timing
   (scanner vs processor vs integrator) before touching anything.
2. **malloc family ~17% of CPU** (ts_malloc + malloc + _int_malloc) —
   allocation traffic in parse+extract. tree-sitter's internal
   allocations dominate; an arena for extractor-side vectors is the
   in-our-control slice (karpathy rule 2).
3. **`ts_language_table_entry` 7.8%** — C# grammar table lookups inside
   tree-sitter; not ours to fix (grammar-dependent).

Theoretical best cold start on dotnet-class corpora ≈ scan (few s) +
parse floor (~8s) + integrate + warmup heuristics (~3s) ≈ **12–15s**,
vs 47s today — the remaining 3x is parallel-efficiency and allocator
work, to be re-profiled after the integrator/queue timing split.
