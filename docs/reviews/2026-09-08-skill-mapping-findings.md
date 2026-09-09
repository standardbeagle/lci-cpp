# Findings from the 2026-09-08 feature-skill mapping

Collected while writing the `lci-*` skills under `.agents/skills/`. Each item was reported by the skill writer for that area and is unverified beyond the cited line unless stated. None are fixed here.

## lci-code-insight
- docs/TOOLS.md:437 analysis= lists 4 values, code accepts 11; `languages` param documented but rejected live; attributes/min_lines/threshold/target/flow undocumented.
- handlers_analysis.cpp:2320 tool description claims "79.8% context reduction" / "2-4x accuracy" with no measurement in docs/ or benchmarks/.
- goldens/mcp/code_insight/basic.json: 4-file corpus emits no MODULES/DEPENDENCIES/LOAD BEARING/CYCLES/VOCABULARY/git sections.
- dogfood gap: no CLI file/dir search (find_files MCP-only).
- docs/reviews/2026-09-03-review.md S11 items already fixed but not marked resolved.
## router
- 15 MCP tools registered; docs/TOOLS.md + README say 14; `callers` undocumented.
## lci-side-effects
- 7ab2c29 keyed results file:line:column but unified_extractor.cpp:391 never passes column; lookups use column 0 -> same-line functions still collide.
- fixpoint_truncated() has no reader in src/mcp or src/cli; silent truncation at MCP surface.
- empty-result hint says `@lci:label=...`; parser accepts only `@lci:labels[...]` (semantic_annotator.cpp:80); pinned in goldens/mcp/semantic_annotations/basic.json.
- category-mode error lists 7 categories, category_name_to_bit accepts 16; kFieldWrite/kAsync/kReflection/kIndirectWrite have no producer.
- SideEffectAnalyzerConfig::trust_annotations, track_field_access never read.
- runtime.cpp:60 propagator seeding by name+line: anonymous functions never seed impure.
- review S? open: git analyzer SymbolInfo.is_pure never filled (docs/reviews/2026-09-03-review.md:241).
## lci-git-analysis
- PatternDetector (src/git/pattern_detector.cpp, 521 lines) has no production caller.
- ChangeFrequencySummary::anti_patterns_found never assigned; emitted at insight_sections.cpp:450 and advertised docs/TOOLS.md:477 -> always 0.
- FrequencyCache, analyze_file, get_collision_risk zero callers; calculate_ownership runs but never emitted.
- CLI text mode omits metrics-issues section though risk_score counts them; -f help omits `metrics`.
- git goldens envelope-only (empty diff / [] / error); no real finding pinned.
- ChangeFrequencyParams + MetricsThresholds unreachable from every surface.
- dogfood: `lci refs` lacks a path exclude filter.
## lci-server-lifecycle
- http-socket-api.mdx:73 documents only `shutdown [--force]`; --all, servers, reaper policies, /mcp, /callers absent. README policy table omits max_rss_mb.
- server.cpp:98-104 stale RssAnon mmap rationale (a9bf1ac moved content to heap; filed 01M1PE7698726F1P9QQMTQ5FT1).
- /stats.num_threads = hardware_concurrency (server_endpoints.cpp:462), labelled "Threads" in lci status.
- root-gone exit only fires when root existed at start() (server.cpp:671-676); observed [root deleted] server alive at 56s idle.
- re-check whether handle_reindex clear()-before-index (01M1PA4DNHVBWX1749E3KQJA5N) is closed.
- dogfood: route table string literals inside lambdas invisible to lci.
## lci-mcp-server
- 14-vs-15 tool count drift in docs/TOOLS.md:3, mcp-server.mdx:3,9, README.md:116,213; no `## callers` section.
- README.md:119-120 says all MCP output is LCF; only code_insight emits LCF.
- README.md:150-153 says embedded MCP server stays out of registry; src/cli/mcp.cpp:139-141 calls enable_instance_registry (deliberate per comment). README stale.
- register_parity_compat_tools deleted (memory stale); unknown-param guard is server.cpp:308-345, validation.cpp is search error-shape only.
- CompactFormatter (formatter_compact.cpp) zero production callers.
- http-socket-api.mdx lacks POST /mcp.
- dogfood: `lci def dispatch_wire` misses out-of-class member def; needs qualified name.
- host: ~/.config/lci/config.kdl sets error_report "capture" (user-level config layer, config.cpp:641).
## lci-benchmarks-evaluation
- 79.8% context-reduction claim unsourced: only in handlers_analysis.cpp:2320 (synced from Go text f4cec4a), karpathy-principles.md:9, README.md:3, index.mdx:36; repo-qa tiers show lci costing 1.5-2.5x agent tokens. "2-4x accuracy" also unsourced.
- README says Version 0.6.0; binary 0.10.1.
- benchmarks/repo-qa/config.kdl:8 lci-bin -> build/src/lci (stale Jul 15 build), not build/release/src/lci.
- corpora.json source_path for scikit-learn and next.js absent locally (only fastapi, trpc).
- SKIP_IF_NO_REAL_PROJECT defined 8 times instead of in tests/helpers/real_project_helpers.h.
- minefield tier computed by lci itself (profile_repos.py): difficulty axis shares mechanism with subject.
- dogfood: preprocessor macros not indexed as symbols.
## lci-ops-diagnostics
- update compares versions by string equality (updater.cpp:459): 0.10.1 dev binary offered downgrade to 0.7.0.
- install.sh:136-139 skips verification when SHA256SUMS absent; updater.cpp:553-560 refuses. Asymmetric.
- packaging/npm + python pinned 0.6.0; no workflow publishes them.
- README runtime deps (libssl, brotli) vs DEB depends only libc6 (CMakeLists.txt:518); binary also links libz, libcrypto3.
- README "~28 MB stripped" vs 33.8 MB local stripped.
- CPACK_PACKAGE_HOMEPAGE_URL -> standardbeagle/lci (CMakeLists.txt:494), should be lci-cpp.
- tests/fuzz/README.md lists 6 targets; CMake builds 8.
- README What-it-does table lists `lci stats`; no such CLI verb is registered in src/cli/main.cpp (only `status`).
- debug deps/graph stubs pending S12 removal.
## lci-indexing-pipeline
- index.watch_mode defaults true but make_kdl_base_config (config.cpp:491, applied :703) resets false + debounce 0 + max_goroutines=4 whenever .lci.kdl exists; Go-parity residue pinned by config_test.cpp:601. Every project with a config silently loses watch mode.
- README.md:158-159 "256-way sharded bucketer -> merger" dead: TrigramMergerPipeline/ShardedTrigramStorage no production caller (pipeline_processor.cpp:301-320); live prefilter = per-file TrigramBloom.
- StringPool/FileStringPool (src/string_pool.cpp) unused in src/.
- DeletedFileTracker write-only in production (filter_candidates/is_deleted never called).
- index.priority_mode, smart_size_control parsed but unread.
- dogfood: `lci refs is_indexing` printed one line twice (duplicate ref rows).
- brief error (mine): content store is copy-at-index, not mmap-retained.
## lci-symbol-navigation
- docs/TOOLS.md:241 documents inspect_symbol.max_depth (dropped in 7949bc6); tool-surface.json:45 still has browse_file.show_imports (known RED pytest).
- /definition and /references are text searches (handlers_search.cpp:141-300); only callers/tree/inspect use resolved edges; docs imply symbol-level lookup.
- /tree emits hard-coded zeros dependent_count/edit_risk_score/impact_radius + null annotations (server_endpoints.cpp:469-590) — karpathy rule 6.
- extractor: C++ function-local vars marked [exported] (callers_report.cpp:34,106); `// namespace` closing comment misattributed as doc.
- goldens/mcp/tree/basic.json pins `unknown tool "tree"`.
- dogfood: no CLI find_files; `lci symbols` lacks flags filter; `lci def` can't take object id.
## lci-get-context
- rich get_context never wires set_graph_propagator/set_semantic_annotator (handlers_get_context.cpp:555; only tests call them) -> propagation_labels/criticality always empty in production though McpRuntime seeds a propagator (runtime.cpp:55-70).
- context load format=signatures|outline parsed (handlers_context.cpp:375) but ignored (context_manifest_expander.cpp:122 commented out).
- context.refs schema advertises l{start,end}/note; wire is l{s,e}/n (handlers_context.cpp:748-760 vs :84-87).
- docs/TOOLS.md:145,149 stale ("engine not ported"); :190 lists `pattern` directive absent, omits interface/siblings/doc/signature.
- integration spec pins `unknown tool "context_manifest"`.
- purity: by-ref parameter reported as global write (apply_context_lookup_mode).
- dogfood: `lci refs ContextLookupEngine` misses constructor call site.
## lci-search
- SemanticConfig/SemanticScoringConfig (config.h:83,90) have no KDL parser branch and zero readers; SearchConfig::enable_fuzzy no-op; default_context_lines not parsed (config.cpp:404-432).
- docs/TOOLS.md:82 max default 50 vs code 15 (handlers_search.cpp:415); :92-97 flat results[] vs live per-file hits[] grouping.
- cli-usage.mdx:27-31 omits --group --patterns --invert-match --template-strings.
- 80% claim: only agent-level literal-search measurement (ANALYSIS-discovery-sweep.md:92) has grep AHEAD (F1 1.000 vs 0.840).
- review S5 nc/iv item already fixed (handlers_search.cpp:433-434), review text stale.
- dogfood: search cannot express a regex over code with quotes/operators well.
## lci-config
- config validate prints flags.config_path (commands_config.cpp:317), not ConfigResult.source from f2c4b2b; golden pins old string.
- dead keys: index.smart_size_control, priority_mode, performance.debounce_ms, startup_delay_ms, all search.* incl ranking + enable_fuzzy, propagation_config_dir; SemanticConfig/SemanticScoringConfig/FeatureFlagsConfig no keys, no readers; search.max_context_lines validated never read.
- consumed but unsettable: performance.parallel_file_workers, indexing_timeout_sec, search.default_context_lines.
- config init template says exclude "extends defaults" but project exclude REPLACES ~110 defaults; cwd guard says "Run lci init" (cli_core.cpp:86), only `lci config init` exists; show -f yaml/kdl silently print table; init -f json writes unread file.
- config show -f json omits server/insight/synonyms/attributes/max_parse_file_size/data_file_token_cap/overflow_policy.
- hidden dirs skipped unconditionally (pipeline_scanner.cpp:164): --include cannot reach .github/.
- three glob matchers + two include/exclude walkers (scanner vs watcher.cpp:140).
- gitignore gaps: .git/info/exclude, core.excludesFile, .ignore; user include/exclude dropped when project file exists.
- synonyms block undocumented in user docs.
## lci-parsing-languages
- README.md:171-173 says 6 langs import resolution; import_resolver.cpp:22-31 also PHP, Zig.
- ExtractionResults.imports has no consumer outside src/parser (two producers for imports).
- handlers_search.cpp:109-128 independent language->ext alias table diverging from kLangMap.
- symbol.h:122 stale "receiver_type has no writer" (reference_tracker.cpp:637 writes it).
- pipeline_processor.cpp:110, git/analyzer.cpp:200 use get_results() copies where take_results() exists.
- tests/fixtures/sample_*.txt dead fixtures.
- goldens/http/fileinfo.json pins empty {"file_info": {}}.
- dogfood: `lci def Class` gives header only; member defs in split TUs need grep.
