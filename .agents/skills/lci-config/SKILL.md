---
name: lci-config
description: "Use when: a question names .lci.kdl, a config key, ~/.config/lci/config.kdl, LCI_* env vars, --include/--exclude/-c, gitignore handling in the index, path attributes, the synonyms block, `lci config init|show|validate`, the KDL parser, or a config comparison against ripgrep/ctags/Zoekt/Cursor/Aider/Claude Code settings."
---

# LCI Configuration

`.lci.kdl` in the project root controls: what to index (include/exclude globs, gitignore, size/count budgets), server behavior (idle exit, instance cap, RSS cap), path attribution (test/vendored/generated/production), synonym groups, and beta analysis gates. `~/.config/lci/config.kdl` supplies defaults under the project file; `LCI_ERROR_REPORT` beats both. Absent a file, lci runs on built-in defaults and refuses to index a cwd that is neither a git repo nor configured.

## Surfaces

| Surface | Entry point | Notes |
|---|---|---|
| `.lci.kdl` (project file) | `src/config/config.cpp:961` `load_config` | `<root>/.lci.kdl`; missing file means defaults, not an error |
| `-c/--config <path>` | `src/cli/main.cpp:73`, `src/cli/cli_core.cpp:48` `load_config_with_overrides` | named file must exist (`load_config_file`, `config.cpp:966`); relative path resolves against cwd, not `--root` |
| `--include <glob>` | `src/cli/main.cpp:76`, `src/cli/cli_core.cpp:105` | REPLACES the config include list |
| `--exclude <glob>` | `src/cli/main.cpp:78`, `src/cli/cli_core.cpp:108` | APPENDS to the config exclude list |
| `-r/--root <dir>` | `src/cli/main.cpp:80`, `src/cli/cli_core.cpp:112` | overrides `project.root` |
| `~/.config/lci/config.kdl` | `src/config/config.cpp:643` `user_config_path`, `:658` `overlay_user_config` | `$XDG_CONFIG_HOME/lci/config.kdl` first; malformed user file is fatal |
| `LCI_ERROR_REPORT=off|capture|on` | `src/config/config.cpp:864` `apply_env_overrides` | invalid value fails config load |
| `LCI_MCP_MODE=1|true` | `src/cli/cli_core.cpp:127` `is_mcp_mode` | forces MCP stdio mode; not a config field |
| `XDG_STATE_HOME` | `src/mcp/handlers_analysis.cpp:2387` | error-report capture dir |
| `XDG_RUNTIME_DIR` | `src/cli/server.cpp:61` | server registry dir |
| `lci config init [-f kdl|json] [-o] [--force] [--minimal]` | `src/cli/main.cpp:666`, `src/cli/commands_config.cpp:33` `run_config_init` | yaml is refused |
| `lci config show [-f table|json]` | `src/cli/main.cpp:693`, `src/cli/commands_config.cpp:198` `run_config_show` | help advertises kdl/yaml; both fall through to table |
| `lci config validate` | `src/cli/main.cpp:706`, `src/cli/commands_config.cpp:283` `run_config_validate` | exit 1 on load/validation error; prints heuristic warnings |
| Spawned server argv | `src/cli/server.cpp:92` `build_server_spawn_argv` | threads `-c`, `--include`, `--exclude` so server and CLI filter identically |
| `.gitignore` (root + nested) | `src/config/gitignore.cpp:9` `load_gitignore` | read when `index.respect_gitignore` (default true) |
| Build manifests (tsconfig outDir, composer vendor-dir, csproj OutputPath) | `src/indexing/generated_artifacts.cpp:134` `derive_generated_excludes` | implicit excludes, no config key |

## Code map

- `include/lci/config.h` — every struct: `ProjectConfig`, `IndexConfig`, `PerformanceConfig`, `ServerConfig`, `SemanticConfig`, `SemanticScoringConfig`, `SearchRankingConfig`, `SearchConfig`, `FeatureFlagsConfig`, `InsightConfig`, `Config`, `ConfigResult` (`error`, `warnings`, `source`), and the API (`load_config`, `load_config_file`, `parse_kdl_content`, `validate_config`, `make_default_config`).
- `src/config/config.cpp` — the loader. `apply_project:195`, `apply_index:211`, `apply_performance:283`, `apply_server:309`, `apply_insight:335`, `apply_ranking:367`, `apply_search:401`, `apply_synonyms:441`, `parse_attributes_node:517`, `apply_kdl_nodes:571` (top-level dispatch, `include`/`exclude`/`propagation_config_dir`), `warn_unknown:128` (unknown keys are warnings), typed setters `set_int/set_int64/set_double/set_bool/set_string` (wrong type is an error), `parse_size_string:160` ("10MB"), `expand_tilde:464`, `make_kdl_base_config:491` (Go-parity base when a file IS present), `make_default_config:724` (the 100+ default exclude globs), `parse_kdl_content:694`, `load_config_from:877`, `validate_config:972` (range checks plus 0-means-auto substitution).
- `include/lci/kdl.h`, `src/core/kdl.cpp` — the one KDL parser (`kdl::parse:355`, `Node`, `Token`, `Property`). Subset: string/number/bool args, `key=value` props, child blocks, `//` and `/* */` comments. No KDL v2 `#true`, no type annotations, no slashdash.
- `src/cli/cli_core.cpp:48` `load_config_with_overrides` — where flags meet the file; also the no-config/no-git cwd guard (`:80`) and warning printing (`:99`).
- `src/cli/commands_config.cpp` — `config init|show|validate` bodies and the two `init` templates (`:70` minimal, `:102` full).
- `include/lci/config/gitignore.h`, `src/config/gitignore.cpp` — `GitignoreParser` (`load_dir`, `add_pattern`, `parse_pattern:86`, `should_ignore:179`, `glob_match:386`). Nested files, negation, anchoring, directory-only, last-match-wins.
- `include/lci/path_classifier.h`, `src/core/path_classifier.cpp` — `PathAttrRegistry`, `PathClassifier`, `AttrDef`, `PathAttrRule`, `Capability`; shipped ruleset `kBuiltinAttributeRuleset:21` (vendored 0, generated 1, test 2, benchmark 3, example 4, docs 5, production 99); `parse_attributes_block:293`, `with_config:456`.
- `include/lci/semantic/synonym_table.h` — `SynonymOp{Group,Clear,ClearAll}`, `SynonymTable::build_default`, `build_from_ops`.
- Consumers: `src/indexing/pipeline_scanner.cpp:47` `FileScanner` ctor (exclude, include, derived excludes, gitignore, attribute registry), `should_include:254`, `should_process_file:262`, budget cut `:96-127`; `src/indexing/watcher.cpp:140` mirrors the filters for live events; `src/indexing/master_index.cpp:49,58,73,75` (token cap, attributes, memory cap, file size); `src/indexing/pipeline.cpp:125` workers; `src/server/server.cpp:928,1056,1087` (max_instances, idle, RSS); `src/mcp/handlers_analysis.cpp:1006,436` (entry_points, synonyms); `src/cli/mcp.cpp:100` (local overrides keep MCP in-process).
- Tests: `tests/config_test.cpp` (101 tests: `KdlConfigTest`, `UserConfigTest`, `LoadConfigTest`, `ValidateConfigTest`, `DefaultConfigTest`, `DefaultExcludeContract`, `DefaultExcludeMatchGlob`), `tests/kdl_test.cpp` (`KdlTest`), `tests/gitignore_test.cpp` (`GitignoreParser`, `GitignoreGitParity` runs real `git check-ignore`, `GlobMatch`), `tests/path_classifier_test.cpp` (`PathClassifierTest`), `tests/cli_test.cpp:43-90,312-490,2731,2773,2873` (`CliConfigTest`, `CliConfigFlagTest`, `CliConfigInitTest`, `CliConfigValidateTest`, `CliConfigShowTest`, `CliConfigGuardTest`, `CliServerSpawnTest`), `tests/fuzz/fuzz_config_kdl.cpp` (libFuzzer over `parse_kdl_content`).
- Goldens: `tests/integration/cli/config/{show,validate}.spec.json`, `tests/integration/cli/config/corpus/.lci.kdl`, `tests/integration/goldens/cli/config/{show,validate}.txt`.
- Docs: `README.md:136` (server block), `docs/src/content/docs/cli-usage.mdx:8` (global flags), `docs/src/content/docs/mcp-server.mdx:106` (attributes block), `docs/src/content/docs/error-handling-score.mdx:40-60` (insight.error_report, user config file, env var), `docs/TOOLS.md:450` (entry_points), `docs/superpowers/specs/2026-05-31-synonym-matching-design.md` (synonyms), `docs/reviews/2026-09-03-review.md:255-280` (S12/S13 findings that drove the latest fixes).

## Config knobs

Key table. "Consumer" is the production read site; "none" means parsed and stored but never read.

| KDL path | Type | Default | Struct field | Consumer |
|---|---|---|---|---|
| `project.root` | string | cwd (no file) / `<root>` | `project.root` | everything; `~` expanded, relative made canonical |
| `project.name` | string | "" | `project.name` | `config show` only |
| `index.max_file_size` | int or "10MB" | 10 MB; validate: 1..100 MB | `index.max_file_size` | `pipeline_scanner.cpp:285`, `master_index.cpp:75,569`, `watcher.cpp:230` |
| `index.max_parse_file_size` | int or size string | 2 MB (0 disables) | `index.max_parse_file_size` | `pipeline_processor.cpp:298` |
| `index.data_file_token_cap` | int | 4096 (0 disables) | `index.data_file_token_cap` | `pipeline_processor.cpp:334`, `master_index.cpp:49` |
| `index.max_total_size_mb` | int | 500 | `index.max_total_size_mb` | `pipeline_scanner.cpp:98` |
| `index.max_file_count` | int | 50000 | `index.max_file_count` | `pipeline_scanner.cpp:100` |
| `index.overflow_policy` | "reduced"/"reject" | "reduced" | `index.overflow_policy` | `pipeline_scanner.cpp:116` |
| `index.follow_symlinks` | bool | false | `index.follow_symlinks` | `pipeline_scanner.cpp:175` |
| `index.smart_size_control` | bool | true | `index.smart_size_control` | none (dead) |
| `index.priority_mode` | string | "recent" | `index.priority_mode` | none (dead; `get_file_priority` is extension-based) |
| `index.respect_gitignore` | bool | true | `index.respect_gitignore` | `pipeline_scanner.cpp:67,167,269`, `watcher.cpp:133,152` |
| `index.watch_mode` | bool | true (no file) / false (file present) | `index.watch_mode` | `server.cpp:237`, `watcher.cpp:81` |
| `index.watch_debounce_ms` | int | 300 (no file) / 0 (file present) | `index.watch_debounce_ms` | `watcher.cpp:255` |
| `performance.max_memory_mb` | int | 500; 0=auto 1024; min 100 | `performance.max_memory_mb` | content-store cap `master_index.cpp:73` |
| `performance.max_goroutines` | int | 0=hw threads (no file) / 4 (file present) | `performance.max_goroutines` | `pipeline.cpp:126` fallback worker count |
| `performance.debounce_ms` | int | 100 | `performance.debounce_ms` | none (watcher reads `index.watch_debounce_ms`) |
| `performance.startup_delay_ms` | int | 1500 / 0 | `performance.startup_delay_ms` | none |
| (not parseable) | int | 0=hw threads | `performance.parallel_file_workers` | `pipeline.cpp:125`; no KDL key reads it |
| (not parseable) | int | 120 / 0 | `performance.indexing_timeout_sec` | `cli/server.cpp:267`; no KDL key reads it |
| `server.idle_timeout_sec` | int | 1800 (0 disables) | `server.idle_timeout_sec` | `server.cpp:1056` |
| `server.max_instances` | int | 8 (0 disables) | `server.max_instances` | `server.cpp:928` |
| `server.max_rss_mb` | int | 4096 (0 disables; Linux only) | `server.max_rss_mb` | `server.cpp:1087` |
| `search.max_results` | int | 100 | `search.max_results` | none outside `config show` |
| `search.max_context_lines` | int | 100; 0=auto 50 | `search.max_context_lines` | none (engine reads `SearchOptions.max_context_lines`) |
| `search.enable_fuzzy` | bool | true | `search.enable_fuzzy` | none; header comment says Go-parity only |
| `search.merge_file_results` | bool | true | `search.merge_file_results` | none (SearchOptions has its own field) |
| `search.ensure_complete_stmt` | bool | false | `search.ensure_complete_stmt` | none |
| `search.include_leading_comments` | bool | true | `search.include_leading_comments` | none |
| `search.ranking.enabled` | bool | true | `search.ranking.enabled` | none |
| `search.ranking.code_file_boost` | double | 50.0 | `search.ranking.code_file_boost` | none |
| `search.ranking.doc_file_penalty` | double | -20.0 | `search.ranking.doc_file_penalty` | none |
| `search.ranking.config_file_boost` | double | 10.0 | `search.ranking.config_file_boost` | none |
| `search.ranking.require_symbol` | bool | false | `search.ranking.require_symbol` | none |
| `search.ranking.non_symbol_penalty` | double | -30.0 | `search.ranking.non_symbol_penalty` | none |
| (not parseable) | int | 0 | `search.default_context_lines` | `SearchEngine` ctor default `kDefaultContextLines`, not config |
| `insight.error_report` | "off"/"capture"/"on" | "off" | `insight.error_report` | `handlers_analysis.cpp:523,2380`, `handlers_side_effects.cpp:731`, `cli/mcp.cpp:237` |
| `insight.entry_points` | string list | [] | `insight.entry_points` | `entry_signatures.cpp:202`, `handlers_analysis.cpp:1006,1136,1521` |
| `include` | string args or block | [] = all supported types | `include` | `pipeline_scanner.cpp:56,254`, `watcher.cpp:156`; empty section is an ERROR |
| `exclude` | string args or block | ~110 globs (`config.cpp:731-850`) | `exclude` | `pipeline_scanner.cpp:53`, `watcher.cpp:124,147`; a project file REPLACES the defaults |
| `propagation_config_dir` | string | "" | `propagation_config_dir` | none |
| `synonyms { group ...; clear <w>; clear-all }` | block | curated dev-verb set | `synonyms` | `SearchEngine` (`cli/mcp.cpp:118`, `cli/server.cpp:351`), `NamingAnalyzer` (`handlers_analysis.cpp:436`, `git/analyzer.cpp:587`) |
| `attributes { <attr> "<pat>"; <attr> rank=N { activates; dir; glob; content } }` | block | shipped ruleset | `attribute_defs`, `attributes` | `pipeline_scanner.cpp:49`, `master_index.cpp:58` |
| `version` | int | 1 | `version` | not parsed from KDL; emitted by `config init -f json` only |
| (no keys) | | | `semantic`, `semantic_scoring`, `feature_flags` | no `apply_*`, no consumer: dead structs |

Env: `LCI_ERROR_REPORT` (config override), `LCI_MCP_MODE` (mode detect), `XDG_CONFIG_HOME`/`HOME`/`USERPROFILE` (user file, `~`), `XDG_STATE_HOME` (capture dir), `XDG_RUNTIME_DIR` (registry). `LCI_PREFIX`/`LCI_VERSION` belong to the install script (`README.md:31`), not the binary.

Layering order (`config.cpp:694-720`, `:877-957`): struct defaults -> `make_kdl_base_config` (only when a project file exists) -> user `config.kdl` -> project file (which first CLEARS include/exclude, so user include/exclude only apply with no project file) -> `--include`/`--exclude`/`--root` -> `LCI_ERROR_REPORT` -> `validate_config`.

## Invariants and traps

- Fail-fast on type and range, warn on unknown keys. Wrong-typed known key is an error (`config.cpp:70-125`); unknown key is a warning printed by `cli_core.cpp:99`, so a newer file loads on an older binary. Empty `include {}` is an error (`config.cpp:600-606`).
- A project file switches the base to Go-parity zero values: `watch_mode` false, `watch_debounce_ms` 0, `max_goroutines` 4, `startup_delay_ms` 0 (`config.cpp:491-515`). A repo with any `.lci.kdl` has watch mode OFF unless it says `watch_mode true`. Test `LoadConfigTest.MissingFilePathKeepsRicherNoFileDefaults` pins the split.
- A project `exclude` block REPLACES the ~110 default globs (`parse_kdl_content` clears before overlay). Templates written by `config init` say "extends defaults" (`commands_config.cpp:141`) which the code does not do. Doc drift.
- Hidden directories are skipped unconditionally at `pipeline_scanner.cpp:164` regardless of include/exclude/gitignore, and `**/.*/**` is also in the default excludes. `--include` cannot reach `.github/`.
- Three glob dialects coexist: `gitignore.cpp:386 glob_match` (shared with the git churn filter), `path_classifier.cpp:139 glob_match` (attribute rules), and `FileScanner::compile_glob/match_glob` (include/exclude). `DefaultExcludeMatchGlob` tests pin the scanner's reading of the defaults.
- gitignore parity is tested against real `git check-ignore` (`tests/gitignore_test.cpp:395`), covering interior-slash anchoring, leading-slash anchoring, wildcard directory patterns, nested files (fixed in `700c1a0`). Not implemented: `.git/info/exclude`, `core.excludesFile`, `.ignore`/`.lciignore` (grep for those names in `src/` returns nothing).
- The scanner mirrors gitignore for the initial walk; the watcher re-implements the filter (`watcher.cpp:140`) with a slightly different include check (rel path OR basename). Two walkers, one grammar: `karpathy-principles.md` rule 4 class.
- `config validate` prints `Config source: <flags.config_path>` (`commands_config.cpp:317`), not `ConfigResult.source` added in `f2c4b2b` for exactly this purpose; the golden `tests/integration/goldens/cli/config/validate.txt` pins the old string. Wiring is open (worktrack `01M1NCSJ31540P7H406WBA729W`).
- `config show -f json` emits 13 keys and omits server, insight, synonyms, attributes, max_parse_file_size, data_file_token_cap, overflow_policy (`commands_config.cpp:209-225`).
- `config init -f json` writes `.lci.kdl.json` with `max_file_count 10000` and a Go-era include list; nothing reads JSON config back (`commands_config.cpp:149-168`).
- The cwd guard error text says "Run `lci init`" (`cli_core.cpp:86`); the command is `lci config init` (`main.cpp:666`). No top-level `init` is registered.
- `SearchConfig.enable_fuzzy` and all `search.ranking.*` keys are parsed and never consumed (header comment `config.h:121-125`; grep in Config knobs). `SemanticConfig`, `SemanticScoringConfig`, `FeatureFlagsConfig` have no KDL keys at all. Ranking constants `kDefaultCodeFileBoost` etc. live in `config.h:104-107` but the engine's rank code does not read `cfg.search.ranking`.
- `respect_gitignore` stays true under Go-parity base (confirmed empirically per comment `config.cpp:495-497`).
- `parse_kdl_content` overlays the real user file on every call; fuzzing pins `XDG_CONFIG_HOME` to a nonexistent dir (`tests/fuzz/fuzz_config_kdl.cpp:13`). Any test calling `parse_kdl_content` inherits the developer's `~/.config/lci/config.kdl` unless it does the same (`UserConfigTest` fixture handles it).
- MCP: `lci mcp` bridges to the shared server only when there are NO local overrides (`-c`, `--include`, `--exclude`, `LCI_ERROR_REPORT`); with any of them it stays in-process (`cli/mcp.cpp:100`), so those overrides never leak into a shared server.
- Reviewer lesson: a criterion for a security or correctness knob states the invariant, not the mechanism (`karpathy-principles.md` rule 3); `config.h:75` RSS-cap rationale was falsified by `a9bf1ac` (mmap -> heap) and is tracked at `01M1PE7698726F1P9QQMTQ5FT1`.
- Dogfood gaps: `lci def load_config -r .` and `lci refs load_config -r .` work. No lci verb reports which config keys a binary reads (a "consumer" query needed grep across `src/`); no `lci config show --effective` distinguishing default/user/project/flag origin per key.

## Probe recipes

Binary: `build/release/src/lci`. Scratch dir `S=$(mktemp -d)`.

```sh
# Effective config for this repo (table / json)
build/release/src/lci -r . config show
build/release/src/lci -r . config show -f json
build/release/src/lci -r . config validate

# Unknown key -> warning on stderr, exit 0; wrong type -> error, exit 1
mkdir -p $S/cfgprobe && cd $S/cfgprobe && git init -q . 2>/dev/null
printf 'index {\n  max_results 5\n}\n' > $S/cfgprobe/.lci.kdl
build/release/src/lci -r $S/cfgprobe config validate
printf 'search {\n  max_results "5"\n}\n' > $S/cfgprobe/.lci.kdl
build/release/src/lci -r $S/cfgprobe config validate; echo exit=$?

# Env override rejected
LCI_ERROR_REPORT=bogus build/release/src/lci -r . config validate; echo exit=$?

# Named config file must exist; relative resolves against cwd
build/release/src/lci -r . -c /nonexistent.kdl config show; echo exit=$?

# Templates
build/release/src/lci config init -o $S/full.kdl --force
build/release/src/lci config init --minimal -o $S/min.kdl --force

# gitignore oracle: compare lci's verdict with git's on a fixture
git -C <repo> check-ignore -v -- <path>
```

Targeted tests (build `lci_tests` only, never a clean rebuild):

```sh
cmake --build build/release --parallel --target lci_tests
build/release/tests/lci_tests --gtest_filter='KdlConfigTest.*:UserConfigTest.*:LoadConfigTest.*:ValidateConfigTest.*:DefaultExclude*.*:KdlTest.*:GitignoreParser.*:GitignoreGitParity.*:GlobMatch.*:PathClassifierTest.*:CliConfig*.*:CliServerSpawnTest.*'
ctest --test-dir build/release -R 'KdlConfigTest|GitignoreParser|KdlTest' -j4
# Goldens (cli/config/show and validate) run inside:
ctest --test-dir build/release -R lci_integration_suite --output-on-failure
# Fuzz target (needs the fuzz preset): fuzz_config_kdl, corpus under tests/fuzz/corpus
```

Goldens: `tests/integration/goldens/cli/config/show.txt`, `validate.txt`; corpus config pins `max_goroutines 8` so `show` is host-independent.

## Product comparison

Comparable tools: ripgrep (`.ripgreprc`, `.ignore`, `.rgignore`, gitignore stack), universal-ctags (`.ctags.d/*.ctags`, `~/.ctags.d`), Sourcegraph/Zoekt (`zoekt-indexserver` flags, repo `-ignore_dirs`, Sourcegraph site config), Cursor (`.cursorignore`, `.cursorindexingignore`), Aider (`.aider.conf.yml`, `.aiderignore`, env `AIDER_*`), Claude Code (`settings.json` user/project/local layering, `permissions.deny` path rules).

lci claims in this area:

| Claim | Where made | How to measure | Existing numbers |
|---|---|---|---|
| Server lifecycle bounded by three `server` keys | `README.md:136-150` | start N servers in temp roots, watch `lci servers`, delete a root, check exit | `tests/integration/server_lifecycle_test.cpp`; memory `server-lifecycle-reaper.md` |
| `LCI_ERROR_REPORT` beats both files; invalid value fails load | `docs/src/content/docs/error-handling-score.mdx:57` | env probe above (`exit=1`) | `KdlConfigTest.RejectsInvalidErrorReportMode` |
| Attributes: config patterns first, then shipped by rank | `include/lci/path_classifier.h:36-38`, `mcp-server.mdx:106-125` | `PathClassifierTest.*`; `KdlConfigTest.Attributes*` | 38 unit tests |
| gitignore semantics match git | `include/lci/config/gitignore.h:70-74` | `GitignoreGitParity.MatchesRealGitCheckIgnore` (real `git check-ignore`) | one oracle test, 12 rows |
| Default excludes never hide first-party tests | `src/config/config.cpp:772-782` | `DefaultExcludeContract.TestPatternsAreNotExcluded` | contract test locks count |
| Budgets: 500 MB / 50k files, reduced or reject | `include/lci/config.h:35-44` | index a corpus above budget with each policy | `pipeline_integration_test.cpp` |

Axes:

| Axis | lci | Competitor check | Notes |
|---|---|---|---|
| Ignore-semantics parity with git | nested `.gitignore`, negation, anchoring, dir-only; oracle test vs `git check-ignore`; no `info/exclude` or global excludes | ripgrep: `rg --debug` shows which rule fired; supports `.gitignore`, `.ignore`, `.rgignore`, global excludes (unverified, from model knowledge). Cursor: `.cursorignore` uses gitignore syntax (unverified, from model knowledge) | lci also hardcodes hidden-dir skip (`pipeline_scanner.cpp:164`) |
| Extra ignore file | none; use `exclude` in `.lci.kdl` or `--exclude` | rg `.ignore`, Cursor `.cursorindexingignore`, Aider `.aiderignore` (unverified, from model knowledge) | gap: no tool-agnostic `.ignore` read |
| Per-project vs global | project `.lci.kdl` over `~/.config/lci/config.kdl`; flags over both; env for one gate | rg `RIPGREP_CONFIG_PATH` single file; ctags `.ctags.d` dirs at home and cwd; Claude Code user/project/local `settings.json` (unverified, from model knowledge) | lci user include/exclude are dropped when a project file exists |
| Validation UX | typed errors with key path; unknown keys warn; `config validate` heuristics; line numbers from KDL parser | rg: bad `.ripgreprc` line errors at startup (unverified, from model knowledge); ctags `--_force-quit` style checks; JSON tools via schema | lci has no schema file to feed an editor |
| Effective-config introspection | `config show` (partial key set) | rg `--debug`; Claude Code `/config` (unverified, from model knowledge) | gap: no per-key origin, missing keys in json |
| Size/count budgets | `max_file_size`, `max_parse_file_size`, `max_total_size_mb`, `max_file_count`, `data_file_token_cap`, `overflow_policy` | rg `--max-filesize`; Zoekt `-max_file_size`/trigram limits (unverified, from model knowledge) | lci's reject policy names the tripped limit |
| Path attributes | data-driven ruleset extensible in config, four capability gates | ctags `--exclude`, Sourcegraph `search.excludedRepos`/file filters; SonarQube `sonar.tests`/`sonar.exclusions` (unverified, from model knowledge) | unique to lci: attribute gates search/refs/analysis separately |
| Semantic synonyms | `synonyms` block, disjoint groups, clear/clear-all | no direct equivalent in grep-class tools | undocumented in user docs (only the design spec) |
| Config language | KDL v1 subset, one hand-written parser | TOML/YAML/JSON elsewhere | KDL v2 `#true` rejected with a named error |

Honest gaps: dead keys (`smart_size_control`, `priority_mode`, `debounce_ms`, `startup_delay_ms`, every `search.*` key, `propagation_config_dir`), three whole structs without keys, `parallel_file_workers`/`indexing_timeout_sec` consumed but unsettable from KDL, no `.ignore` file, no global gitignore, no schema/LSP completion for `.lci.kdl`, `config show` incomplete, user include/exclude ignored under a project file, `exclude` replaces rather than extends (contrary to the template comment).

## Related skills

lci-feature-map (router), lci-search, lci-symbol-navigation, lci-get-context, lci-parsing-languages, lci-code-insight, lci-side-effects, lci-git-analysis, lci-indexing-pipeline, lci-server-lifecycle, lci-mcp-server, lci-ops-diagnostics, lci-benchmarks-evaluation
