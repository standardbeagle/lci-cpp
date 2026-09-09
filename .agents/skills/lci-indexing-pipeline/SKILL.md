---
name: lci-indexing-pipeline
description: "Use when: tracing how lci scans/excludes/parses files into the index, debugging reindex or watch-mode staleness, reading RSS/memory numbers, changing gitignore/budget behaviour, or comparing lci indexing to Zoekt, ripgrep, ctags, clangd, Cursor-style indexes."
---

# LCI Indexing Pipeline

lci builds an in-memory code index per project root: a scanner walks the tree honouring `.gitignore`, default excludes and a corpus budget; worker threads parse each file with tree-sitter and tokenize it; a serial integrator merges symbols, references, trigram blooms and postings into RCU-published snapshots that readers consume lock-free. There is no on-disk index: every server start re-indexes, and a watch path (efsw) applies single-file updates afterwards.

## Surfaces

| Surface | Entry point | Notes |
|---|---|---|
| `lci server` / `lci mcp` startup build | `src/server/server.cpp:505` (`indexer_->index_directory`) | Runs in a swapped indexing thread; on success starts the watch pipeline (`server.cpp:528`) |
| `POST /reindex` | `src/server/server_endpoints.cpp:301` (`IndexServer::handle_reindex`) | Validates `path` inside project root, 503 while shutting down; calls `index_directory` at `:397` without a pre-`clear()` |
| `GET/POST /status`, `/stats` | `src/server/server_endpoints.cpp:106` (`handle_status`); routes `src/server/server.cpp:1173`, `:1197`, `:1260`, `:1264` | Reports `IndexingPhase` (`idle`/`scanning`/`indexing`/`merging`) and progress |
| `lci status [-j] [-v]` | `src/cli/main.cpp:410`; impl `src/cli/status.cpp` | Client view of `/status` |
| `lci list [-v]` (alias `ls`) | `src/cli/main.cpp:650`; `src/cli/commands.cpp:816` (`run_list`) | Prints what the scanner would index; cheapest way to audit excludes |
| `lci debug info` | `src/cli/main.cpp:722` | Server-side numbers only; `--incremental` is rejected (golden `tests/integration/goldens/cli/debug/info-incremental.txt`) |
| `lci debug memprofile [--ratio --floor-mb --top]` | `src/cli/main.cpp:806`; `src/cli/memprofile.cpp` (`run_debug_memprofile`) | Single-threaded per-file memory attribution through the real processor + integrator |
| MCP `index_stats` | `src/mcp/handlers_index.cpp:431` (registration), `:102` (`handle_index_stats`) | `status`, `progress`, readiness flags; documented in `docs/TOOLS.md` |
| MCP `debug_info` | `src/mcp/handlers_index.cpp:462`, `:183` (`handle_debug_info`) | Index internals |
| `.lci.kdl` `index {}` / `performance {}` / `server {}` / `include` / `exclude` / `attributes` | `src/config/config.cpp:214-320`, `:593-625` | See Config knobs |
| Watch mode (no verb) | `src/server/server.cpp:236` (`start_watch_pipeline`), `src/indexing/watcher.cpp:251` (`WatchPipeline`) | Runs automatically when `index.watch_mode` is true and the bulk build succeeded |

No MCP tool triggers a reindex; only the HTTP route does.

## Code map

Entry -> core -> storage, in the order a bulk build runs.

- `src/indexing/master_index.cpp` / `include/lci/indexing/master_index.h` - `MasterIndex::index_directory` (`:157`) owns the bulk window: `bulk_mu_`, `set_bulk_indexing(true)` before any sub-index is touched, buffered scan+parse, commit path clears staging generations (`:265-268`), `publish_snapshot` (`:776`), `retain_only` prune. `index_file` (`:511`), `update_file` (`:561`), `remove_file` (`:605`) are the incremental writers; `clear()` (`:635`). `FileSnapshot` (header `:31`) carries path<->FileID maps plus per-file `PathAttrId` and the precomputed searchable id list. `IndexingPhase` enum at header `:175`.
- `src/indexing/master_index_search.cpp` - read side over the same snapshots (`search_with_options`, `find_candidate_files`, `searchable_file_ids`); no locks.
- `src/indexing/pipeline.cpp` / `include/lci/indexing/pipeline.h` - `Pipeline::scan_and_parse` (`:31`): scan, batched pre-load into the content store in scan order for deterministic FileIDs, `BoundedQueue<FileTask>` to workers, `BoundedQueue<ProcessedFile>` back; `Pipeline::integrate` (`:219`). Worker count from `performance.parallel_file_workers` then `max_goroutines`, 0 = auto. Stage timings logged as `lci: index stages:` (`:200`, `:230`).
- `src/indexing/pipeline_scanner.cpp` / `include/lci/indexing/pipeline_scanner.h` - `FileScanner::scan(apply_budget)` (`:72`): walk, priority sort (ext tiers at `:290`), byte + file budget, `overflow_policy` reduced/reject. `should_process_file` (`:262`) order: binary extension, gitignore, exclude, include, attribute `Capability::Index`, `max_file_size`. `CompiledGlob` literal prefilter (`:206`). `walk_directory` (`:137`) tracks inodes for symlink cycles and avoids `fs::relative`. `derive_generated_excludes` is merged into exclusions at `:63`.
- `src/config/gitignore.cpp` / `include/lci/config/gitignore.h` - `GitignoreParser::load_gitignore` loads root plus nested `.gitignore` files with per-directory `base`; `should_ignore` (`:179`), `fast_match` (`:266`) with literal prefilter; `glob_match` is the shared glob dialect (also used by the git churn filter).
- `src/indexing/generated_artifacts.cpp` / `include/lci/indexing/generated_artifacts.h` - `derive_generated_excludes(root)`: reads tsconfig/jsconfig `outDir`, composer `vendor-dir`, `*.csproj` output paths into root-relative globs.
- `src/indexing/binary_detector.cpp` / `include/lci/indexing/binary_detector.h` - `BinaryDetector::is_binary_by_extension`, `is_binary_by_magic_number` (second check runs on loaded content in the processor).
- `src/core/path_classifier.cpp` / `include/lci/path_classifier.h` - `PathClassifier`, `PathAttrRegistry` (builtin ruleset `kBuiltinAttributeRuleset`, `with_config`). Attributes (production/test/vendored/generated/...) are data with `Capability` gates `Index`, `Search`, `Refs`, `Analysis`. Classified once on the write path, stored in `FileSnapshot::file_attrs`.
- `src/indexing/pipeline_processor.cpp` / `include/lci/indexing/pipeline_processor.h` - `FileProcessor::worker_loop`, `process_file` (`:248`): magic-number binary check, tree-sitter parse via `ParserPool` (`include/lci/parser/parser_pool.h`) and `UnifiedExtractor` skipped above `max_parse_file_size`, per-file `TrigramBloom` build, parallel postings tokenization with `postings_token_cap`. `ParseSkipReason` in `include/lci/indexing/pipeline_types.h:59` names why extraction produced nothing.
- `src/indexing/pipeline_integrator.cpp` / `include/lci/indexing/pipeline_integrator.h` - `FileIntegrator::integrate_file`: `merge_trigrams` (installs bloom or marks unfiltered), `merge_symbols` (ReferenceTracker imports, symbols, enrichment, `SymbolLocationIndex`), `merge_postings` (pre-tokenized merge), `remove_stale_data`.
- `src/indexing/pipeline_progress.cpp` - `ProgressTracker` counters behind `/status`.
- `src/core/trigram.cpp` / `include/lci/core/trigram.h` - `TrigramIndex` (RCU `mutate_snapshot`, `narrow`, `Narrowing::certifies_absent`, `set_bulk_indexing`), `TrigramBloom::build`, `ShardedTrigramStorage` (256 buckets, `:438`). `src/core/trigram_bucketing.cpp` `bucket_trigrams`.
- `src/indexing/trigram_merger.cpp` / `include/lci/indexing/trigram_merger.h` - `TrigramMergerPipeline` (default 16 workers). See traps: no production caller.
- `src/core/file_content_store.cpp` / `include/lci/core/file_content_store.h` - `FileContentStore` RCU snapshot of owned per-file byte vectors, `batch_add_files`, `retain_only`, `enforce_memory_limit` (sized from `performance.max_memory_mb`, `master_index.cpp:72`). `include/lci/core/file_service.h` `FileService::batch_load_from_disk` maps each file transiently (`include/lci/core/mmap.h` `MappedFile`) and copies.
- `src/core/reference_tracker.cpp` / `include/lci/core/reference_tracker.h` - `ReferenceTracker` (symbols, `StoredRef` pinned to 32 bytes at `:380`), `PostingsIndex` (`:161`). `include/lci/core/symbol_store.h` `SymbolLocationIndex` (`:156`).
- `src/string_pool.cpp` / `include/lci/string_pool.h` - `StringPool`, `FileStringPool` interning. See traps.
- `src/indexing/watcher.cpp` / `include/lci/indexing/watcher.h` - `FileWatcher` (efsw adapter, exclude/gitignore filtering before dispatch), `WatchPipeline::on_event` (`:284`, efsw thread, queue only), `apply_path` (`:326`), `on_rebuild` (`:352`, timer thread, the only writer). `src/indexing/debounced_rebuilder.cpp` `DebouncedRebuilder` (per-FileID debounce, `kPathBatchId` batch for removes/new paths). `src/indexing/deleted_file_tracker.cpp` `DeletedFileTracker` (CAS snapshots).
- `src/server/server.cpp:223-262` - watch pipeline registry (`start_watch_pipeline`, `stop_watch_pipeline`); `:1049` `reaper_loop` with the RSS self-cap (`read_own_rss_mb` `:105`, `malloc_trim` then loud exit).
- `src/cli/memprofile.cpp` - memory attribution tool; `src/cli/commands.cpp:816` `run_list`.
- Tests: `tests/pipeline_test.cpp` (FileScannerTest, FileScannerBudgetTest, GeneratedArtifactsTest, GitignoreParserTest, BinaryDetectorTest, FileProcessorTest, MaxParseFileSize, PipelineTest), `tests/master_index_test.cpp` (MasterIndexTest incl. `UpdateDuringBulkWindowSurvivesExactlyOnce` `:635`, `ReindexKeepsPriorSnapshotReadableUntilPublish`, `CancelledReindexReturnsFalseAndKeepsPriorGeneration`, `ThrowDuringReindexUnwindsIndexingStateAndAllowsRetry`), `tests/integrator_test.cpp`, `tests/index_performance_test.cpp`, `tests/gitignore_test.cpp` (GitignoreGitParity runs real `git check-ignore`), `tests/path_classifier_test.cpp`, `tests/file_content_store_test.cpp`, `tests/watcher_test.cpp` (`WiredPipelineIndexesNewFunctionWithoutReindex`), `tests/trigram_test.cpp`, `tests/config_test.cpp` (DefaultExcludeContract), `tests/integration/pipeline_integration_test.cpp` (`RespectGitignore`, `ReIndexingProducesSameResults`), `tests/soak/index_size_smoke_test.cpp`, `tests/soak/soak_memory_test.cpp`.
- Goldens: `tests/integration/goldens/http/reindex.json`, `status.json`, `stats.json`; `tests/integration/goldens/mcp/index_stats/basic.json`; `tests/integration/goldens/cli/status/`.
- Benchmarks: `tests/benchmarks/real_project_benchmarks.cpp` (`BM_RealProjectIndexChi`, `BM_RealProjectIndexPocketbase`), baseline `tests/benchmarks/baseline/linux-x64.json` (`BM_IndexingThroughput/*`, `BM_TrigramIndexFile`, `BM_ParserPoolAcquireRelease`).
- Docs: `README.md:158-167` (architecture), `docs/src/content/docs/http-socket-api.mdx:78` (in-memory only), `docs/plans/2026-04-30-runtime-storage-redesign-design.md`, `docs/plans/2026-08-30-cold-start-profiling.md`, `docs/plans/2026-08-17-ast-retention-ir-design.md`, `docs/performance/profiling-wsl2.md`, `docs/performance/integration-index-cache.md`, `docs/reviews/2026-09-03-review.md` (S1-S4 index lifecycle findings).

## Config knobs

All in `.lci.kdl`; struct fields in `include/lci/config.h`. No `LCI_*` environment variable affects indexing (`grep getenv src` finds only `LCI_MCP_MODE`, `LCI_ERROR_REPORT`).

| Key | Default | Field | Effect |
|---|---|---|---|
| `index.max_file_size` | 10 MB | `IndexConfig::max_file_size` | Scanner drops larger files |
| `index.max_parse_file_size` | 2 MB | `IndexConfig::max_parse_file_size` | Larger files are trigram/postings-indexed but not parsed; 0 disables |
| `index.data_file_token_cap` | 4096 | `IndexConfig::data_file_token_cap` | Unique-token cap for non-code files; capped files marked PARTIAL and self-nominate |
| `index.max_total_size_mb` / `index.max_file_count` | 500 / 50000 | `IndexConfig::max_total_size_mb`, `max_file_count` | Corpus budget spent in priority order |
| `index.overflow_policy` | `reduced` | `IndexConfig::overflow_policy` | `reduced` truncates and reports skips; `reject` fails the run (validated `config.cpp:986`) |
| `index.respect_gitignore` | true | `IndexConfig::respect_gitignore` | |
| `index.follow_symlinks`, `smart_size_control`, `priority_mode` | false / true / `recent` | `IndexConfig` | `priority_mode` and `smart_size_control` are parsed but no scanner code reads them (grep `src/indexing`) |
| `index.watch_mode` | true (struct); **false when a `.lci.kdl` exists** | `IndexConfig::watch_mode` | `make_kdl_base_config` (`config.cpp:491`, applied at `:703`) resets to Go's zero value; pinned by `tests/config_test.cpp:601` |
| `index.watch_debounce_ms` | 300 (struct); 0 with a `.lci.kdl` | `IndexConfig::watch_debounce_ms` | Same Go-parity reset |
| `performance.parallel_file_workers` / `max_goroutines` | 0 auto / 0 auto (4 with a `.lci.kdl`) | `PerformanceConfig` | Worker count, `pipeline.cpp:87-90` |
| `performance.max_memory_mb` | 500 | `PerformanceConfig::max_memory_mb` | `FileContentStore` eviction budget |
| `server.max_rss_mb` | 4096 | `ServerConfig::max_rss_mb` | Reaper reads RssAnon, `malloc_trim`, exits loudly if still over |
| `include { ... }` / `exclude { ... }` | defaults `config.cpp:730-790` | `Config::include`, `Config::exclude` | Empty `include` section is an error; default excludes cover dot-dirs, `build-*`, lockfiles, minified bundles, fonts |
| `attributes { ... }` | shipped ruleset | `Config::attribute_defs`, `Config::attributes` | Per-attribute `activates "index" "search" ...` gates |

## Invariants and traps

- Bulk window opens before any sub-index is touched; readers keep the old generation until `publish_snapshot`. Clearing before the window published four empty snapshots and made the live server answer "not found" for the whole reindex (`src/indexing/master_index.cpp:190-204`; `docs/reviews/2026-09-03-review.md` S1).
- Lock order is `bulk_mu_ -> snapshot_mu_` everywhere. Incremental writers take `bulk_mu_` first and wait out the whole bulk run; a liveness check instead of the lock was a TOCTOU (`.claude/rules/karpathy-principles.md` rule 3, `master_index.h:81-87`).
- `clear()` on any RCU store publishes an empty generation immediately. Never call `MasterIndex::clear()` before `index_directory` on a live server; `handle_reindex` used to and no longer does (`server_endpoints.cpp:393`).
- Cancellation is failure: `request_stop` discards the buffered parse and returns false, the prior generation survives. A thrown exception unwinds `is_indexing_` via the guard (`master_index.cpp:179-187`).
- The efsw callback thread must never write to the index; every event goes through `DebouncedRebuilder`'s timer thread (`watcher.h:109-125`, karpathy rule 3 corollary).
- FileIDs are path-stable across a reindex; the content store is pruned with `retain_only` only after publish so a stale id never aliases another file (`master_index.cpp:198-203`, `:319-330`).
- FileID assignment is deterministic because the producer pre-loads in scan order (priority desc, path asc). Do not let workers call `load_file_from_disk` first (`pipeline.cpp:38-50`; karpathy rule 4).
- Content is COPIED out of a transient mmap at index time (`file_content_store.h:26-43`, `file_service.h:32-44`). The comment at `src/server/server.cpp:97-101` still says the store retains mmaps; that premise is false since `a9bf1ac` and the RssAnon cap now counts content bytes (`docs/reviews/2026-09-03-review.md:141-146`, filed `01M1PE7698726F1P9QQMTQ5FT1`).
- Per-occurrence trigram bucketing and `TrigramMergerPipeline` have no production caller: `FileIntegrator::enable_merger_pipeline` is called only from headers/tests, and the processor comment records the removal (`pipeline_processor.cpp:301-320`). The live prefilter is the per-file `TrigramBloom`.
- Findings from the 2026-09-08 mapping (fixed or filed): see docs/reviews/2026-09-08-skill-mapping-findings.md.
- `StringPool` / `FileStringPool` (`src/string_pool.cpp`) have no users under `src/` other than their own TU; only `tests/error_string_pool_test.cpp` exercises them.
- `DeletedFileTracker` is written by `WatchPipeline::on_rebuild` and exposed via `deleted_files()`, but no search path calls `filter_candidates` or `is_deleted` (grep `src include`); removal correctness comes from `remove_file`, not from the tracker.
- Test and fixture directories are indexed on purpose (grep parity); filter at query time (`config.cpp:772-780`).
- Files that load then fail (binary magic, parse bail) consume a FileID without entering the file map, so never derive counts from `1..N` (`master_index.cpp:291-295`).
- `max_file_count` diverges from the retired Go default (50000 vs 10000) deliberately (`config.cpp:488-490`).
- Memory scales with symbol/reference density, not file count: next.js 19k files -> 547 MB peak; dotnet subset 16k files / 597k symbols -> 3.8 GB, under the 4096 MB self-cap; raising the budget to 55k files segfaulted (`docs/plans/2026-08-29-code-insight-multi-repo-qa.md:177-195`). Ratios quoted in memory notes (1.6x self, 5.5x next.js) are historical and not in `docs/`.
- Memory attribution: VmRSS for phase totals, glibc `mallinfo2` for per-file deltas; RSS deltas per file are arena-chunking noise (`src/cli/memprofile.cpp:10-16`).
- Dogfood gap: `lci refs is_indexing -r .` printed `master_index.cpp:667` twice (duplicate reference rows for one line).

## Probe recipes

Binary: `build/release/src/lci`. Root flag `-r <root>`.

```
# What would be indexed (audit excludes/gitignore/attributes)
build/release/src/lci list -r . | wc -l
build/release/src/lci list -v -r . | head

# Index status and phase (starts a server if none)
build/release/src/lci status -j -r .
build/release/src/lci debug info -r .

# Cold index wall + peak RSS for a corpus (whole process, one-shot server)
/usr/bin/time -v build/release/src/lci server --foreground -r <corpus>   # watch stderr for "lci: index stages:"

# Per-file memory attribution
build/release/src/lci debug memprofile --ratio 10 --floor-mb 5 --top 20 -r <corpus>

# Trigger reindex over the unix socket (path must be inside the root).
# Socket is <tmpdir>/lci-<uid>-<roothash>.sock (src/server/server.cpp:187-192); `lci servers` lists root -> socket.
build/release/src/lci servers
curl --unix-socket /tmp/lci-$(id -u)-<roothash>.sock -X POST http://lci/reindex -d '{}'

# Navigation
build/release/src/lci def index_directory -r .
build/release/src/lci refs set_bulk_indexing -r .
```

Targeted tests (build `lci_tests` only):

```
cmake --build build/release --parallel --target lci_tests
build/release/tests/lci_tests --gtest_filter='MasterIndexTest.*:FileScanner*:GitignoreParser*:GitignoreGitParity*:GeneratedArtifactsTest.*:FileContentStore*:FileWatcherTest.*:DebouncedRebuilderTest.*:PathClassifierTest.*'
ctest --test-dir build/release -R 'lci_integration_suite|lci_index_size_smoke|lci_memory_soak' --output-on-failure
```

Goldens live in `tests/integration/goldens/{http,mcp,cli}`; the reindex golden substitutes `${CORPUS}` for the absolute path (`.claude/rules/worktree-isolation-and-goldens.md` rule 2 explains the worktree false failure).

Bulk-window race test shape: `set_post_parse_hook` parks the bulk thread, the test races `update_file`, then joins (`tests/master_index_test.cpp:635`).

## Product comparison

Comparable tools: Zoekt (persistent trigram shards, Sourcegraph's indexer), Sourcegraph SCIP indexers, ripgrep (no index, parallel scan), Google codesearch / `csearch` (persistent trigram index), universal-ctags tag files, clangd background index (`.cache/clangd/index` shards), Cursor / Continue embedding indexes (remote or local vector stores), GitHub code search (server-side sparse n-gram index).

lci claims in this area:

| Claim | Where made | How to measure | Existing numbers |
|---|---|---|---|
| In-memory index rebuilt on server start; no persistence | `README.md:160-161`, `docs/src/content/docs/http-socket-api.mdx:78` | Restart server, watch `index stages:` on stderr; `docs/performance/integration-index-cache.md:65` proves no cross-process reuse | Cold index of 10 test corpora 24.8 s (`integration-index-cache.md:51-58`) |
| Lock-free reads during reindex | `README.md:162-165`, `master_index.h:27-30` | `MasterIndexTest.ReindexKeepsPriorSnapshotReadableUntilPublish`; `ConcurrentReadsWhileUpdating` | tests only |
| Cold start on large corpora | `docs/plans/2026-08-30-cold-start-profiling.md:29-36` | `/usr/bin/time -v lci server --foreground -r <corpus>` best of 3 | dotnet 151 s -> 47.3 s, RSS 3.81 -> 1.93 GB; next.js 23.8 -> 14.4 s |
| RSS budget <= 2x corpus | `docs/plans/2026-08-17-ast-retention-ir-design.md:18-20` | RssAnon from `/proc/<pid>/status` after bulk vs corpus bytes from `lci list` sizes | Self-cap and multi-repo table in `2026-08-29-code-insight-multi-repo-qa.md:177-185` |
| Watch mode serves fresh results without `/reindex` | `watcher.h:104-107` | `FileWatcherTest.WiredPipelineIndexesNewFunctionWithoutReindex`; live: edit a file, `lci def` the new symbol after `watch_debounce_ms` | tests only; no latency number in docs |
| Deterministic ids and output across runs | `pipeline.cpp:38-43`, karpathy rule 4 | `PipelineIntegrationTest.ReIndexingProducesSameResults` | tests only |

Comparison axes:

| Axis | lci | Checking the competitor | Notes |
|---|---|---|---|
| Cold index time | Parallel tree-sitter parse + trigram + postings, in memory; `index stages:` stderr line | Zoekt `zoekt-index` wall on the same tree; `csearch` `cindex`; clangd background index completion in logs; ripgrep has no index step | lci pays parse cost that pure text indexers do not (parse was 53% of dotnet index CPU, `cold-start-profiling.md:13`) |
| RSS per corpus byte | Owned content copy + symbols + refs + blooms + postings; scales with symbol density | Zoekt shard size on disk and RSS of `zoekt-webserver`; ripgrep near zero resident | Zoekt keeps shards on disk and mmaps them (unverified, from model knowledge) |
| Incremental update latency | efsw event -> debounce (`watch_debounce_ms`) -> single-file reparse under `bulk_mu_`/`snapshot_mu_` | Zoekt reindexes whole repos on a schedule; clangd reindexes a TU on save; ctags needs a re-run | lci has no measured number for update latency |
| Persistence | None; rebuild every start | Zoekt/csearch/clangd/ctags all persist to disk | Documented gap; `integration-index-cache.md:69-82` rejected building one until a serializer exists |
| Exclusion fidelity | Nested `.gitignore` parity tested against `git check-ignore`; manifest-derived generated excludes; attribute gates | ripgrep honours `.gitignore`/`.ignore`/`.rgignore`; Zoekt uses its own skip list | `GitignoreGitParity` requires `git` on PATH |
| Budget / overflow behaviour | 500 MB / 50k files, priority-ordered truncation or reject, reported in `/status` | Zoekt has per-shard size limits; ripgrep unbounded | lci's `priority_mode` and `smart_size_control` keys are inert |
| Determinism | Sorted scan, producer-assigned ids, sort before emit | Compare two runs' outputs byte-wise | Hash-order bugs must be tested across processes (karpathy rule 4) |

Honest gaps: no disk persistence, so every server start and every `lci mcp` session pays a full cold index; no published incremental-update latency; the merger/sharded trigram path in README does not run; memory for reference-dense corpora approaches the 4 GB self-cap; `priority_mode`/`smart_size_control` are accepted but unused; watch mode silently turns off for any project that ships a `.lci.kdl` unless it sets `watch_mode true`.

## Related skills

lci-feature-map (router), lci-search, lci-symbol-navigation, lci-get-context, lci-parsing-languages, lci-code-insight, lci-side-effects, lci-git-analysis, lci-server-lifecycle, lci-mcp-server, lci-config, lci-ops-diagnostics, lci-benchmarks-evaluation
