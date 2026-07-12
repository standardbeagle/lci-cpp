#include <lci/cli/commands.h>
#include <lci/indexing/pipeline_scanner.h>
#include <lci/version.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#include <CLI/CLI.hpp>

#include "profiling.h"

int main(int argc, char* argv[]) {
    using namespace lci::cli;

    CLI::App app{"Lightning fast code indexing for AI assistants"};
    app.set_version_flag("--version,-V",
                         std::string("lci version ") + lci::kVersion);

    // -- Global flags ---------------------------------------------------------
    // Fully qualified: the Win32 SDK (winbase.h, pulled in transitively by
    // CLI11 on Windows) declares a global GlobalFlags() function that otherwise
    // collides with lci::cli::GlobalFlags under the using-directive above.
    lci::cli::GlobalFlags gflags;
    app.add_option("-c,--config", gflags.config_path, "Config file path")
        ->default_val(".lci.kdl");
    app.add_flag("-d,--daemon", gflags.daemon, "Run as daemon");
    app.add_option("--include", gflags.include,
                   "Include files matching glob patterns");
    app.add_option("--exclude", gflags.exclude,
                   "Exclude files matching glob patterns");
    app.add_option("-r,--root", gflags.root,
                   "Project root directory to index (overrides config)");
    app.add_flag("--test-run", gflags.test_run,
                 "Show files that would be indexed without processing")
        ->group("");  // hidden
    // Hidden global profile flags (Go parity: cmd/lci/main.go:108-115). When
    // gperftools is linked, these write CPU/heap profiles. When not, the
    // runtime path fails fast with a clear error — no silent no-op.
    app.add_option("--profile-cpu", gflags.profile_cpu_path,
                   "Write CPU profile to file (requires gperftools)")
        ->group("");
    app.add_option("--profile-memory", gflags.profile_memory_path,
                   "Write memory (heap) profile to file (requires gperftools)")
        ->group("");

    // -- Search subcommand ----------------------------------------------------
    auto* search_cmd =
        app.add_subcommand(
               "search",
               "Search for pattern in codebase (literal substring match; "
               "whitespace is part of the pattern, not a word separator)")
            ->alias("s");

    std::string search_pattern;
    search_cmd->add_option("pattern", search_pattern,
                           "Search pattern (literal substring — "
                           "'foo bar' matches the exact 6-char string)")
        ->required();

    // Optional trailing path args (ripgrep `rg pattern [path...]`). A single
    // string positional followed by a vector positional: CLI11 gives `pattern`
    // the first token and `paths` every remaining positional.
    std::vector<std::string> search_paths;
    search_cmd->add_option("paths", search_paths,
                           "Optional files or directory prefixes to scope "
                           "results to (e.g. src/ or src/foo.cpp)");

    int search_max_lines = 0;
    search_cmd->add_option("-m,--max-lines", search_max_lines,
                           "Max context lines (0=use blocks)");

    bool search_case_insensitive = false;
    search_cmd->add_flag("-i,--case-insensitive", search_case_insensitive,
                         "Case-insensitive search");

    bool search_json = false;
    search_cmd->add_flag("-j,--json", search_json, "Output as JSON");

    bool search_light = false;
    search_cmd->add_flag("--light", search_light,
                         "Use light search without relational data");

    std::string search_exclude;
    search_cmd->add_option("-e,--exclude", search_exclude,
                           "Exclude files matching regex pattern");

    std::string search_include;
    search_cmd->add_option("--inc,--include", search_include,
                           "Include only files matching regex pattern");

    bool search_comments_only = false;
    search_cmd->add_flag("--comments-only", search_comments_only,
                         "Search only in comments");

    bool search_code_only = false;
    search_cmd->add_flag("--code-only", search_code_only,
                         "Search only in code");

    bool search_strings_only = false;
    search_cmd->add_flag("--strings-only", search_strings_only,
                         "Search only in string literals");

    bool search_invert_match = false;
    search_cmd->add_flag("--invert-match", search_invert_match,
                         "Show lines that don't match");

    // Multiple patterns OR'd together (grep -e). `--patterns` long-only
    // because the positional arg `pattern` already occupies that name in
    // CLI11's flag namespace.
    std::vector<std::string> search_patterns;
    search_cmd->add_option("--patterns", search_patterns,
                           "Additional patterns OR'd with the positional "
                           "pattern (grep -e). Repeat for each extra pattern.");

    bool search_count = false;
    search_cmd->add_flag("--count", search_count,
                         "Return match count per file");

    bool search_files_only = false;
    search_cmd->add_flag("-l,--files-with-matches", search_files_only,
                         "Return only filenames with matches");

    bool search_word_boundary = false;
    search_cmd->add_flag("-w,--word-regexp", search_word_boundary,
                         "Match whole words only");

    bool search_regex = false;
    search_cmd->add_flag(
        "-E,--regex", search_regex,
        "Interpret pattern as RE2 regex. Patterns with a >=3-char "
        "literal substring use the trigram-seeded fast path "
        "(sub-millisecond candidate-set lookup, RE2 verification on "
        "rows). Pure-meta patterns like '\\d+' or '^[a-z]+$' fall back "
        "to a full-corpus RE2 scan (slower but correct).");

    int search_max_count = 0;
    search_cmd->add_option("--max-count", search_max_count,
                           "Max matches per file (0=unlimited)");

    bool search_ids = false;
    search_cmd->add_flag("--ids", search_ids, "Include object IDs in results");

    bool search_no_ids = false;
    search_cmd->add_flag("--no-ids", search_no_ids,
                         "Exclude object IDs from results");

    bool search_compact = false;
    search_cmd->add_flag("--compact-search,--cs", search_compact,
                         "Show compact output");

    std::string search_rank_by;
    search_cmd->add_option(
        "--rank-by", search_rank_by,
        "Rank results by: relevance | recency | file-type. "
        "Go-parity aliases accepted: proximity | similarity (both map "
        "to relevance — see src/cli/rank_options.h _rationale).");

    // Go-parity search-local flags (cmd/lci/main.go:168-188). Each one is
    // honored (not just parsed) — see callback + run_search wiring below.
    bool search_template_strings = false;
    search_cmd->add_flag(
        "--template-strings", search_template_strings,
        "When combined with --strings-only, also match template strings "
        "(sql``, gql``, etc.)");

    bool search_verbose = false;
    // Long-only `--verbose` (no -v short — `-v` is grep's invert-match).
    search_cmd->add_flag("--verbose", search_verbose,
                         "Show debug information on stderr");

    bool search_compare_search = false;
    search_cmd->add_flag(
        "--compare-search", search_compare_search,
        "A/B-compare legacy vs consolidated search path (Go-parity flag; "
        "C++ port has only one path — emits a stderr notice)");

    std::string search_cpu_profile_path;
    search_cmd->add_option("--cpu-profile", search_cpu_profile_path,
                           "Write search-local CPU profile to file "
                           "(requires gperftools)");

    std::string search_mem_profile_path;
    search_cmd->add_option("--mem-profile", search_mem_profile_path,
                           "Write search-local memory profile to file "
                           "(requires gperftools)");

    std::string search_context_filter;
    search_cmd->add_option("--context-filter", search_context_filter,
                           "Filter results by enclosing context: "
                           "function | class | top-level");

    search_cmd->callback([&]() {
        SearchCommandOptions options{
            .pattern = search_pattern,
            .paths = search_paths,
            .max_lines = search_max_lines,
            .case_insensitive = search_case_insensitive,
            .json_output = search_json,
            .light = search_light,
            .compact = search_compact,
            .use_regex = search_regex,
            .exclude_pattern = search_exclude,
            .include_pattern = search_include,
            .invert_match = search_invert_match,
            .extra_patterns = search_patterns,
            .count_per_file = search_count,
            .files_only = search_files_only,
            .word_boundary = search_word_boundary,
            .max_count_per_file = search_max_count,
            .include_ids = search_ids,
            .no_ids = search_no_ids,
            .comments_only = search_comments_only,
            .code_only = search_code_only,
            .strings_only = search_strings_only,
            .rank_by = search_rank_by,
            .context_filter = search_context_filter,
            .template_strings = search_template_strings,
            .verbose = search_verbose,
            .compare_search = search_compare_search,
            .cpu_profile_path = search_cpu_profile_path,
            .mem_profile_path = search_mem_profile_path,
        };
        std::exit(run_search(gflags, options));
    });

    // -- Grep subcommand ------------------------------------------------------
    auto* grep_cmd =
        app.add_subcommand("grep", "Ultra-fast text search")
            ->alias("g");

    std::string grep_pattern;
    grep_cmd->add_option("pattern", grep_pattern, "Search pattern")
        ->required();

    // Optional trailing path args (ripgrep `rg pattern [path...]`). String
    // positional then vector positional — `pattern` takes the first token,
    // `grep_paths` collects the rest.
    std::vector<std::string> grep_paths;
    grep_cmd->add_option("paths", grep_paths,
                         "Optional files or directory prefixes to scope "
                         "results to (e.g. sklearn/utils or foo.py)");

    int grep_max_results = 500;
    grep_cmd->add_option("-n,--max-results", grep_max_results,
                         "Max number of results");

    // Default 0 (off): match-line only, parity with Go's `displayGrepResults`
    // which ignores --context in text mode. Users opt in with `--context N`,
    // and N>0 enables grep -C-style before/after context rendering.
    int grep_context = 0;
    grep_cmd->add_option("-C,--context", grep_context,
                         "Lines of context around matches (grep -C N)");

    bool grep_case_insensitive = false;
    grep_cmd->add_flag("-i,--case-insensitive", grep_case_insensitive,
                       "Case-insensitive search");

    bool grep_json = false;
    grep_cmd->add_flag("-j,--json", grep_json, "Output as JSON");

    std::string grep_exclude;
    grep_cmd->add_option("-e,--exclude", grep_exclude,
                         "Exclude files matching regex pattern");

    std::string grep_include;
    grep_cmd->add_option("--inc,--include", grep_include,
                         "Include only files matching regex pattern");

    bool grep_exclude_tests = false;
    grep_cmd->add_flag("--exclude-tests", grep_exclude_tests,
                       "Exclude test files");

    bool grep_exclude_comments = false;
    grep_cmd->add_flag("--exclude-comments", grep_exclude_comments,
                       "Exclude matches in comments");

    bool grep_regex = false;
    // Short form `-E` mirrors Go's `lci search -E` and GNU grep -E. The
    // previous `-R` alias is dropped to avoid an LCI-only short form that
    // diverged from Go (decision in S3 task spec). `--regex` long form is
    // unchanged so existing scripts keep working.
    grep_cmd->add_flag("-E,--regex", grep_regex,
                       "Interpret pattern as extended (RE2) regex. "
                       "Honored: when set, the positional pattern and all "
                       "--patterns entries are compiled with RE2 and matched "
                       "line-by-line against the result rows returned by the "
                       "literal-match engine.");

    bool grep_verbose = false;
    // Long-only `--verbose`. `-v` is grep's invert-match (decision: keep
    // grep convention, surface `--verbose` separately). Documented divergence
    // from Go's `-v`=verbose in cmd/lci/main.go:297.
    // _rationale: grep -v is universally understood as invert-match across
    // every grep implementation users may already know; aliasing it to verbose
    // would silently break grep muscle memory. Long-only `--verbose` is the
    // honest compromise — discoverable in --help, no shadow on -v.
    grep_cmd->add_flag("--verbose", grep_verbose,
                       "Show debug information on stderr");

    // -- Grep-compatible filter flags (parity with `lci search` and Go CLI) --
    bool grep_invert_match = false;
    grep_cmd->add_flag("-v,--invert-match", grep_invert_match,
                       "Show lines that don't match (grep -v)");

    // `--patterns` only — Go's CLI also accepts `--pattern` as an alias, but
    // CLI11 treats positional + flag names as the same namespace, so we'd
    // collide with the required positional `pattern` argument above.
    std::vector<std::string> grep_patterns;
    grep_cmd->add_option("--patterns", grep_patterns,
                         "Additional patterns OR'd with the positional pattern "
                         "(grep -e). Repeat the flag for each extra pattern.");

    bool grep_count = false;
    grep_cmd->add_flag("-c,--count", grep_count,
                       "Print match count per file instead of match lines "
                       "(grep -c)");

    bool grep_files_only = false;
    grep_cmd->add_flag("-l,--files-with-matches", grep_files_only,
                       "Print only filenames containing matches (grep -l)");

    int grep_max_count = 0;
    grep_cmd->add_option("-M,--max-count", grep_max_count,
                         "Stop after N matches per file (grep -m, 0=unlimited)");

    grep_cmd->callback([&]() {
        std::exit(run_grep(gflags, grep_pattern, grep_max_results,
                           grep_context, grep_case_insensitive, grep_json,
                           grep_exclude, grep_include, grep_exclude_tests,
                           grep_exclude_comments, grep_regex,
                           grep_invert_match, grep_patterns, grep_count,
                           grep_files_only, grep_max_count, grep_verbose,
                           grep_paths));
    });

    // -- Status subcommand ----------------------------------------------------
    auto* status_cmd =
        app.add_subcommand("status", "Show index server status")
            ->alias("st");

    bool status_json = false;
    status_cmd->add_flag("-j,--json", status_json, "Output as JSON");

    bool status_verbose = false;
    status_cmd->add_flag("-v,--verbose", status_verbose,
                         "Show detailed status information");

    status_cmd->callback([&]() {
        std::exit(run_status(gflags, status_json, status_verbose));
    });

    // -- Server subcommand ----------------------------------------------------
    auto* server_cmd =
        app.add_subcommand("server", "Start persistent index server")
            ->alias("srv");

    // Command-local --daemon/--foreground (Go cmd/lci/main.go:801-811). When
    // both are passed, --foreground wins and we emit a stderr notice.
    bool server_daemon = false;
    server_cmd->add_flag("-d,--daemon", server_daemon,
                         "Run as daemon (background, detaches from terminal)");

    bool server_foreground = false;
    server_cmd->add_flag("--foreground", server_foreground,
                         "Force foreground operation (overrides --daemon; "
                         "for debugging)");

    server_cmd->callback([&]() {
        std::exit(run_server(gflags, server_daemon, server_foreground));
    });

    // -- Shutdown subcommand --------------------------------------------------
    auto* shutdown_cmd =
        app.add_subcommand("shutdown", "Shutdown the persistent index server")
            ->alias("stop");

    bool shutdown_force = false;
    shutdown_cmd->add_flag("-f,--force", shutdown_force,
                           "Force shutdown even if operations are in progress");

    shutdown_cmd->callback([&]() {
        std::exit(run_shutdown(gflags, shutdown_force));
    });

    // -- MCP subcommand -------------------------------------------------------
    auto* mcp_cmd = app.add_subcommand(
        "mcp", "Start MCP (Model Context Protocol) server with stdio transport");

    mcp_cmd->callback([&]() { std::exit(run_mcp(gflags)); });

    // -- Update subcommand ----------------------------------------------------
    auto* update_cmd = app.add_subcommand(
        "update", "Update lci to the latest release (self-update)");

    bool update_check = false;
    update_cmd->add_flag("--check", update_check,
                         "Report current vs latest version without installing");

    bool update_force = false;
    update_cmd->add_flag("-f,--force", update_force,
                         "Reinstall even when already up to date");

    std::string update_version;
    update_cmd->add_option("--version", update_version,
                           "Install a specific release version (e.g. 0.6.0) "
                           "instead of the latest");

    update_cmd->callback([&]() {
        std::exit(run_update(update_check, update_force, update_version));
    });

    // Helper: emit a real --test-run file list using the same FileScanner the
    // indexer would consume. Mirrors Go's `MasterIndex.TestRun` (which calls
    // `ListFiles`) — stdout: one path per line; stderr: "Total: N files
    // would be indexed". Replaces the prior stub that just printed
    // "Test run mode: would index from <root>" with no actual file list.
    auto run_test_run = [&]() -> int {
        lci::Config cfg;
        std::string err = load_config_with_overrides(gflags, cfg);
        if (!err.empty()) {
            std::cerr << "Error: " << err << "\n";
            return 1;
        }
        std::fprintf(stderr,
                     "Test run mode: showing files that would be indexed\n");
        // Same scanner the indexer pipeline uses, so the list reflects the
        // real include/exclude/root filters — not a parallel implementation.
        lci::FileScanner scanner(cfg);
        auto tasks = scanner.scan();
        // Sort by path for deterministic output (Karpathy rule 4) and to
        // match Go's `filepath.Walk` lexical order.
        std::sort(tasks.begin(), tasks.end(),
                  [](const lci::FileTask& a, const lci::FileTask& b) {
                      return a.path < b.path;
                  });
        for (const auto& task : tasks) {
            std::printf("%s\n", task.path.c_str());
        }
        std::fprintf(stderr,
                     "\nTotal: %zu files would be indexed\n", tasks.size());
        return 0;
    };

    // -- Def subcommand -------------------------------------------------------
    auto* def_cmd =
        app.add_subcommand("def", "Find symbol definition")
            ->alias("d");

    std::string def_symbol;
    def_cmd->add_option("symbol", def_symbol, "Symbol name")->required();

    def_cmd->callback([&]() { std::exit(run_def(gflags, def_symbol)); });

    // -- Refs subcommand ------------------------------------------------------
    auto* refs_cmd =
        app.add_subcommand("refs", "Find symbol references")
            ->alias("r");

    std::string refs_symbol;
    refs_cmd->add_option("symbol", refs_symbol, "Symbol name")->required();
    bool refs_json = false;
    refs_cmd->add_flag("--json", refs_json, "Output as JSON")->group("");
    bool refs_all = false;
    refs_cmd->add_flag("--all", refs_all,
                       "Include lexical-only matches (strings/comments/"
                       "docstrings) in a separate section");

    refs_cmd->callback([&]() {
        std::exit(run_refs(gflags, refs_symbol, refs_json, refs_all));
    });

    // -- Tree subcommand ------------------------------------------------------
    auto* tree_cmd =
        app.add_subcommand("tree",
                           "Display function call hierarchy tree")
            ->alias("t");

    std::string tree_function;
    tree_cmd->add_option("function", tree_function, "Function name")
        ->required();

    int tree_max_depth = 5;
    tree_cmd->add_option("-d,--max-depth", tree_max_depth,
                         "Maximum recursion depth for call tree");

    bool tree_json = false;
    tree_cmd->add_flag("-j,--json", tree_json, "Output as JSON");

    bool tree_show_lines = true;
    tree_cmd->add_flag("--show-lines", tree_show_lines,
                       "Show line numbers for each call");

    bool tree_compact = false;
    tree_cmd->add_flag("--compact", tree_compact, "Use compact output format");

    std::string tree_exclude;
    tree_cmd->add_option("-e,--exclude", tree_exclude,
                         "Exclude files matching regex pattern");

    bool tree_agent = false;
    tree_cmd->add_flag("--agent", tree_agent,
                       "Output dense dependency data optimized for agents");

    bool tree_metrics = false;
    tree_cmd->add_flag("--metrics", tree_metrics,
                       "Show complexity metrics for each function");

    tree_cmd->callback([&]() {
        std::exit(run_tree(gflags, tree_function, tree_max_depth,
                           tree_show_lines, tree_compact, tree_json,
                           tree_agent, tree_metrics, tree_exclude));
    });

    // -- List subcommand ------------------------------------------------------
    auto* list_cmd =
        app.add_subcommand("list", "List files that would be indexed")
            ->alias("ls");

    bool list_verbose = false;
    list_cmd->add_flag("-v,--verbose", list_verbose,
                       "Show file details (size, priority)");

    list_cmd->callback([&]() {
        std::exit(run_list(gflags, list_verbose));
    });

    // -- Config subcommand ----------------------------------------------------
    auto* config_cmd = app.add_subcommand("config",
                                          "Configuration management commands");

    auto* config_init_cmd =
        config_cmd->add_subcommand("init",
                                   "Initialize configuration file (.lci.kdl)")
            ->alias("i");

    std::string config_init_format = "kdl";
    config_init_cmd->add_option("-f,--format", config_init_format,
                                "Output format: kdl, yaml, json");

    std::string config_init_output;
    config_init_cmd->add_option("-o,--output", config_init_output,
                                "Output file path");

    bool config_init_force = false;
    config_init_cmd->add_flag("--force", config_init_force,
                              "Overwrite existing configuration file");

    bool config_init_minimal = false;
    config_init_cmd->add_flag("--minimal", config_init_minimal,
                              "Generate minimal config");

    config_init_cmd->callback([&]() {
        std::exit(run_config_init(gflags, config_init_format,
                                  config_init_output, config_init_force,
                                  config_init_minimal));
    });

    auto* config_show_cmd =
        config_cmd->add_subcommand("show", "Show current configuration values")
            ->alias("s");

    std::string config_show_format = "table";
    config_show_cmd->add_option("-f,--format", config_show_format,
                                "Output format: kdl, yaml, json, table");

    config_show_cmd->callback([&]() {
        std::exit(run_config_show(gflags, config_show_format));
    });

    auto* config_validate_cmd =
        config_cmd
            ->add_subcommand("validate", "Validate configuration file")
            ->alias("v");

    config_validate_cmd->callback([&]() {
        std::exit(run_config_validate(gflags));
    });

    // -- Debug subcommand -----------------------------------------------------
    auto* debug_cmd =
        app.add_subcommand(
               "debug",
               "Debug and diagnostic tools for symbol linking system")
            ->alias("dbg");

    auto* debug_info_cmd =
        debug_cmd
            ->add_subcommand("info", "Show comprehensive debug information")
            ->alias("i");

    bool debug_info_verbose = false;
    debug_info_cmd->add_flag("-v,--verbose", debug_info_verbose,
                             "Enable verbose debug output");

    bool debug_info_incremental = false;
    debug_info_cmd->add_flag("--incremental,--inc", debug_info_incremental,
                             "Show incremental-index introspection "
                             "(falls back to full scan with notice when "
                             "no snapshot is available)");

    debug_info_cmd->callback([&]() {
        std::exit(run_debug_info(gflags, debug_info_verbose,
                                 debug_info_incremental));
    });

    auto* debug_validate_cmd =
        debug_cmd->add_subcommand("validate", "Validate system consistency")
            ->alias("v");

    bool debug_validate_incremental = false;
    debug_validate_cmd->add_flag("--incremental,--inc",
                                 debug_validate_incremental,
                                 "Run incremental-mode consistency checks");

    debug_validate_cmd->callback([&]() {
        std::exit(run_debug_validate(gflags, debug_validate_incremental));
    });

    auto* debug_deps_cmd =
        debug_cmd->add_subcommand("deps", "Analyze dependency graph")
            ->alias("dependencies");

    bool debug_deps_verbose = false;
    debug_deps_cmd->add_flag("-v,--verbose", debug_deps_verbose,
                             "Show detailed dependency information");

    debug_deps_cmd->callback([&]() {
        std::exit(run_debug_deps(gflags, debug_deps_verbose));
    });

    auto* debug_export_cmd =
        debug_cmd
            ->add_subcommand("export", "Export debug information to JSON")
            ->alias("e");

    std::string debug_export_output = "debug-info.json";
    debug_export_cmd->add_option("-o,--output", debug_export_output,
                                 "Output file for exported debug information");

    bool debug_export_verbose = false;
    debug_export_cmd->add_flag("-v,--verbose", debug_export_verbose,
                               "Show export preview");

    bool debug_export_incremental = false;
    debug_export_cmd->add_flag("--incremental,--inc",
                               debug_export_incremental,
                               "Export the incremental-index delta instead "
                               "of the full snapshot");

    debug_export_cmd->callback([&]() {
        std::exit(run_debug_export(gflags, debug_export_output,
                                   debug_export_verbose,
                                   debug_export_incremental));
    });

    auto* debug_graph_cmd =
        debug_cmd
            ->add_subcommand("graph",
                             "Export dependency graph in DOT format")
            ->alias("g");

    std::string debug_graph_output = "dependency-graph.dot";
    debug_graph_cmd->add_option("-o,--output", debug_graph_output,
                                "Output file for dependency graph");

    debug_graph_cmd->callback([&]() {
        std::exit(run_debug_graph(gflags, debug_graph_output));
    });

    // -- Git-analyze subcommand -----------------------------------------------
    auto* ga_cmd =
        app.add_subcommand("git-analyze",
                           "Analyze git changes for duplicates and naming")
            ->alias("ga");

    std::string ga_scope = "staged";
    ga_cmd->add_option("-s,--scope", ga_scope,
                       "What to analyze: staged, wip, commit, range");

    std::string ga_base;
    ga_cmd->add_option("-b,--base", ga_base,
                       "Base reference for comparison");

    std::string ga_target;
    ga_cmd->add_option("-t,--target", ga_target,
                       "Target reference for range scope");

    std::vector<std::string> ga_focus;
    ga_cmd->add_option("-f,--focus", ga_focus,
                       "Focus analysis: duplicates, naming");

    double ga_threshold = 0.8;
    ga_cmd->add_option("--threshold", ga_threshold,
                       "Similarity threshold (0.0-1.0)");

    int ga_max_findings = 20;
    ga_cmd->add_option("-m,--max-findings", ga_max_findings,
                       "Maximum findings per category");

    bool ga_json = false;
    ga_cmd->add_flag("-j,--json", ga_json, "Output as JSON");

    ga_cmd->callback([&]() {
        std::exit(run_git_analyze(gflags, ga_scope, ga_base, ga_target,
                                  ga_focus, ga_threshold, ga_max_findings,
                                  ga_json));
    });

    // -- Symbols subcommand ---------------------------------------------------
    auto* sym_cmd =
        app.add_subcommand("symbols",
                           "List and filter symbols in the index")
            ->alias("sym");

    std::string sym_kind = "all";
    sym_cmd->add_option("-k,--kind", sym_kind,
                        "Symbol kinds: func, type, struct, interface, method, "
                        "class, enum, variable, constant, all");

    bool sym_exported = false;
    sym_cmd->add_flag("--exported", sym_exported,
                      "Show only exported symbols");

    std::string sym_file;
    sym_cmd->add_option("-f,--file", sym_file,
                        "Glob pattern for file path filter");

    std::string sym_name;
    sym_cmd->add_option("-n,--name", sym_name,
                        "Substring filter on symbol name");

    std::string sym_receiver;
    sym_cmd->add_option("--receiver", sym_receiver,
                        "Filter methods by receiver type");

    int sym_min_complexity = 0;
    sym_cmd->add_option("--min-complexity", sym_min_complexity,
                        "Minimum cyclomatic complexity");

    int sym_max_complexity = 0;
    sym_cmd->add_option("--max-complexity", sym_max_complexity,
                        "Maximum cyclomatic complexity");

    std::string sym_sort = "name";
    sym_cmd->add_option("-s,--sort", sym_sort,
                        "Sort by: name, complexity, refs, line, params");

    int sym_max = 50;
    sym_cmd->add_option("-m,--max", sym_max, "Maximum results");

    bool sym_json = false;
    sym_cmd->add_flag("-j,--json", sym_json, "Output as JSON");

    sym_cmd->callback([&]() {
        std::exit(run_symbols(gflags, sym_kind, sym_exported, sym_file,
                              sym_name, sym_receiver, sym_min_complexity,
                              sym_max_complexity, sym_sort, sym_max,
                              sym_json));
    });

    // -- Inspect subcommand ---------------------------------------------------
    auto* insp_cmd =
        app.add_subcommand("inspect", "Deep inspect a symbol by name or ID")
            ->alias("insp");

    std::string insp_name;
    insp_cmd->add_option("name", insp_name, "Symbol name")->required();

    std::string insp_type;
    insp_cmd->add_option("--type", insp_type,
                         "Symbol type to disambiguate");

    std::string insp_file;
    insp_cmd->add_option("-f,--file", insp_file,
                         "File path pattern to disambiguate");

    std::string insp_include = "all";
    insp_cmd->add_option("--include", insp_include,
                         "Sections to include (comma-separated)");

    bool insp_json = false;
    insp_cmd->add_flag("-j,--json", insp_json, "Output as JSON");

    insp_cmd->callback([&]() {
        std::exit(run_inspect(gflags, insp_name, insp_type, insp_file,
                              insp_include, insp_json));
    });

    // -- Browse subcommand ----------------------------------------------------
    auto* browse_cmd =
        app.add_subcommand("browse",
                           "Browse all symbols in a file (outline view)")
            ->alias("br");

    std::string browse_file;
    browse_cmd->add_option("file", browse_file, "File path")->required();

    std::string browse_kind;
    browse_cmd->add_option("-k,--kind", browse_kind,
                           "Filter by symbol kinds");

    bool browse_exported = false;
    browse_cmd->add_flag("--exported", browse_exported,
                         "Show only exported symbols");

    std::string browse_sort = "line";
    browse_cmd->add_option("-s,--sort", browse_sort,
                           "Sort by: line, name, type, complexity, refs");

    bool browse_imports = false;
    browse_cmd->add_flag("--imports", browse_imports, "Show import list");

    bool browse_stats = false;
    browse_cmd->add_flag("--stats", browse_stats,
                         "Show file-level statistics");

    bool browse_json = false;
    browse_cmd->add_flag("-j,--json", browse_json, "Output as JSON");

    browse_cmd->callback([&]() {
        std::exit(run_browse(gflags, browse_file, browse_kind,
                             browse_exported, browse_sort, browse_imports,
                             browse_stats, browse_json));
    });

    // -- Default action: search or MCP auto-detect ----------------------------
    app.callback([&]() {
        if (app.get_subcommands().empty() ||
            app.get_subcommands().front()->get_name().empty()) {
            if (gflags.test_run) {
                std::exit(run_test_run());
            }

            if (is_mcp_mode()) {
                std::exit(run_mcp(gflags));
            }
        }
    });

    // -- Global CPU/memory profiling --------------------------------------
    //
    // gperftools profilers are started BEFORE parse() because CLI11 invokes
    // subcommand callbacks inside parse() and most callbacks call std::exit()
    // (matching Go's `os.Exit(1)` on error). If we wait until after parse,
    // the profiler never gets a chance to start.
    //
    // We don't have CLI11's parsed values yet at this point, so we scan argv
    // directly for the two profile flags. Side note: a full pre-pass would
    // be fragile (we'd be reimplementing CLI11's parser); a targeted scan
    // for two specific long-only flags is cheap and correct. The flags are
    // hidden (`->group("")`) and never exposed in --help — only users who
    // explicitly need profiling reach for them.
    //
    // Fails FAST (non-zero exit, clear error) when gperftools is missing —
    // per Karpathy rule 6, no silent no-op for a flag the user explicitly
    // set.
    std::string cpu_profile_path_arg;
    std::string mem_profile_path_arg;
    for (int i = 1; i < argc - 1; ++i) {
        std::string a = argv[i];
        if (a == "--profile-cpu") {
            cpu_profile_path_arg = argv[i + 1];
        } else if (a == "--profile-memory") {
            mem_profile_path_arg = argv[i + 1];
        }
    }
    // Static storage so guard destructors run via atexit — every subcommand
    // callback below ends with std::exit(), which DOES invoke atexit handlers
    // but does NOT unwind the stack (no automatic destructors). Putting the
    // guards in static storage and registering an explicit atexit that resets
    // them gives us a deterministic flush on every exit path.
    static lci::cli::ProfilerGuard cpu_guard;
    static lci::cli::ProfilerGuard mem_guard;
    if (!cpu_profile_path_arg.empty() || !mem_profile_path_arg.empty()) {
        std::string err;
        cpu_guard = lci::cli::start_cpu_profile(cpu_profile_path_arg, err);
        if (!err.empty()) {
            std::cerr << "Error: " << err << "\n";
            return 1;
        }
        mem_guard = lci::cli::start_memory_profile(mem_profile_path_arg, err);
        if (!err.empty()) {
            std::cerr << "Error: " << err << "\n";
            return 1;
        }
        std::atexit([]() {
            // Reset via move-assign of a default-constructed guard, which
            // triggers ~ProfilerGuard on the held state (Stop+flush). Safe
            // to call multiple times; the second call is a no-op because
            // the guards are already Inactive after the first reset.
            cpu_guard = lci::cli::ProfilerGuard{};
            mem_guard = lci::cli::ProfilerGuard{};
        });
    }

    // Manual parse + exit-code remap. CLI11's default `ExtrasError` exit
    // is 109; Go's urfave/cli exits 1 on `flag provided but not defined`
    // (see `lci search --enhanced add` against the Go reference binary).
    // Match Go by collapsing every CLI parse failure to exit 1 — keeps
    // shell pipelines that branch on non-zero behaving identically across
    // the two binaries, and matches `if err != nil { os.Exit(1) }` shape.
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        int rc = app.exit(e);
        return rc == 0 ? 0 : 1;
    }

    // cpu_guard / mem_guard destruct on return, flushing the profile.
    return 0;
}
