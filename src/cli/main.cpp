#include <lci/cli/commands.h>
#include <lci/version.h>

#include <cstdlib>
#include <iostream>
#include <string>

#include <CLI/CLI.hpp>

int main(int argc, char* argv[]) {
    using namespace lci::cli;

    CLI::App app{"Lightning fast code indexing for AI assistants"};
    app.set_version_flag("--version,-V", lci::kVersion);

    // -- Global flags ---------------------------------------------------------
    GlobalFlags gflags;
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

    // -- Search subcommand ----------------------------------------------------
    auto* search_cmd =
        app.add_subcommand("search", "Search for pattern in codebase")
            ->alias("s");

    std::string search_pattern;
    search_cmd->add_option("pattern", search_pattern, "Search pattern")
        ->required();

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
    search_cmd->add_flag("-E,--regex", search_regex,
                         "Interpret pattern as extended regex");

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
    search_cmd->add_option("--rank-by", search_rank_by,
                           "Rank results by: relevance | recency | file-type");

    std::string search_context_filter;
    search_cmd->add_option("--context-filter", search_context_filter,
                           "Filter results by enclosing context: "
                           "function | class | top-level");

    search_cmd->callback([&]() {
        std::exit(run_search(
            gflags, search_pattern, search_max_lines, search_case_insensitive,
            search_json, search_light, search_compact, search_regex,
            search_exclude, search_include, search_invert_match,
            search_patterns, search_count,
            search_files_only, search_word_boundary, search_max_count,
            search_ids, search_no_ids, search_comments_only, search_code_only,
            search_strings_only, search_rank_by, search_context_filter));
    });

    // -- Grep subcommand ------------------------------------------------------
    auto* grep_cmd =
        app.add_subcommand("grep", "Ultra-fast text search")
            ->alias("g");

    std::string grep_pattern;
    grep_cmd->add_option("pattern", grep_pattern, "Search pattern")
        ->required();

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
    grep_cmd->add_flag("-R,--regex", grep_regex,
                       "Interpret pattern as extended regex");

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
                           grep_files_only, grep_max_count));
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

    server_cmd->callback([&]() { std::exit(run_server(gflags)); });

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

    refs_cmd->callback([&]() {
        std::exit(run_refs(gflags, refs_symbol, refs_json));
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

    debug_info_cmd->callback([&]() {
        std::exit(run_debug_info(gflags, debug_info_verbose));
    });

    auto* debug_validate_cmd =
        debug_cmd->add_subcommand("validate", "Validate system consistency")
            ->alias("v");

    debug_validate_cmd->callback([&]() {
        std::exit(run_debug_validate(gflags));
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

    debug_export_cmd->callback([&]() {
        std::exit(
            run_debug_export(gflags, debug_export_output, debug_export_verbose));
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
                lci::Config cfg;
                std::string err = load_config_with_overrides(gflags, cfg);
                if (!err.empty()) {
                    std::cerr << "Error: " << err << "\n";
                    std::exit(1);
                }
                std::cerr << "Test run mode: would index from "
                          << cfg.project.root << "\n";
                std::exit(0);
            }

            if (is_mcp_mode()) {
                std::exit(run_mcp(gflags));
            }
        }
    });

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

    return 0;
}
