#include <lci/cli/commands.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <lci/indexing/pipeline_scanner.h>
#include <nlohmann/json.hpp>

namespace lci {
namespace cli {

namespace fs = std::filesystem;

// -- def command --------------------------------------------------------------

int run_def(const GlobalFlags& flags, const std::string& symbol) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::string conn_err;
    auto client = ensure_server_running(cfg, conn_err);
    if (!client) {
        std::cerr << "Error: " << conn_err << "\n";
        return 1;
    }

    std::string def_err;
    auto results = client->get_definition(symbol, 100, def_err);
    if (!results) {
        std::cerr << "Error: definition search failed: " << def_err << "\n";
        return 1;
    }

    for (const auto& r : *results) {
        if (!r.signature.empty()) {
            std::printf("%s:%d: %s\n", r.file_path.c_str(), r.line,
                        r.signature.c_str());
        } else {
            std::printf("%s:%d: %s %s\n", r.file_path.c_str(), r.line,
                        r.type.c_str(), r.name.c_str());
        }
    }

    return 0;
}

// -- refs command -------------------------------------------------------------

int run_refs(const GlobalFlags& flags, const std::string& symbol,
             bool json_output) {
    if (json_output) {
        std::cout
            << "Incorrect Usage: flag provided but not defined: -json\n\n"
            << "NAME:\n"
            << "   lci refs - Find symbol references\n\n"
            << "USAGE:\n"
            << "   lci refs command [command options] \n\n"
            << "COMMANDS:\n"
            << "   help, h  Shows a list of commands or help for one command\n\n"
            << "OPTIONS:\n"
            << "   --help, -h  show help\n";
        return 1;
    }

    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::string conn_err;
    auto client = ensure_server_running(cfg, conn_err);
    if (!client) {
        std::cerr << "Error: " << conn_err << "\n";
        return 1;
    }

    std::string refs_err;
    auto results = client->get_references(symbol, 100, refs_err);
    if (!results) {
        std::cerr << "Error: references search failed: " << refs_err << "\n";
        return 1;
    }

    for (const auto& r : *results) {
        if (!r.context.empty()) {
            std::printf("%s:%d: %s\n", r.file_path.c_str(), r.line,
                        r.context.c_str());
        } else {
            std::printf("%s:%d: %s\n", r.file_path.c_str(), r.line,
                        r.match_text.c_str());
        }
    }

    return 0;
}

// -- tree command -------------------------------------------------------------

int run_tree(const GlobalFlags& flags, const std::string& function_name,
             int max_depth, bool show_lines, bool compact, bool json_output,
             bool agent_mode, bool /*metrics*/, const std::string& exclude) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::string conn_err;
    auto client = ensure_server_running(cfg, conn_err);
    if (!client) {
        std::cerr << "Error: " << conn_err << "\n";
        return 1;
    }

    auto start = std::chrono::steady_clock::now();

    TreeRequest req;
    req.function_name = function_name;
    req.max_depth = max_depth;
    req.show_lines = show_lines;
    req.compact = compact;
    req.exclude = exclude;
    req.agent_mode = agent_mode;

    std::string tree_err;
    auto tree = client->get_tree(req, tree_err);
    if (!tree) {
        std::cerr << "Error: " << tree_err << "\n";
        return 1;
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    double elapsed_ms =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
                .count()) /
        1000.0;

    if (json_output) {
        nlohmann::json output;
        output["function"] = function_name;
        output["time_ms"] = elapsed_ms;
        output["tree"] = *tree;
        std::cout << output.dump(2) << "\n";
        return 0;
    }

    std::printf("Function call tree for '%s' (generated in %.1fms)\n\n",
                function_name.c_str(), elapsed_ms);
    std::cout << tree->dump(2) << "\n";
    return 0;
}

// -- list command -------------------------------------------------------------

int run_list(const GlobalFlags& flags, bool verbose) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    // List files that *would* be indexed by walking the project root through
    // the same FileScanner used by the indexing pipeline. This matches the Go
    // implementation (cmd/lci listCommand → MasterIndex.ListFiles), which also
    // performs a stand-alone scan rather than querying the running server.
    //
    // Output contract (Go-compatible):
    //   - stdout: one absolute file path per line; verbose mode appends
    //     "(priority: N, size: B bytes)".
    //   - stderr: "\nTotal: N files would be indexed\n" summary in non-verbose
    //     mode (parity descriptors only capture stdout, but we keep it for
    //     interactive parity with the Go binary).
    FileScanner scanner(cfg);
    auto tasks = scanner.scan();

    // FileScanner returns tasks sorted by indexing priority (desc) then path
    // (asc); the Go list command emits files in lexical scan order
    // (filepath.Walk), so re-sort by path here for parity.
    std::sort(tasks.begin(), tasks.end(),
              [](const FileTask& a, const FileTask& b) {
                  return a.path < b.path;
              });

    for (const auto& task : tasks) {
        if (verbose) {
            std::printf("%s (priority: %d, size: %lld bytes)\n",
                        task.path.c_str(), task.priority,
                        static_cast<long long>(task.size));
        } else {
            std::printf("%s\n", task.path.c_str());
        }
    }

    if (!verbose) {
        std::fprintf(stderr, "\nTotal: %zu files would be indexed\n",
                     tasks.size());
    }
    return 0;
}

// -- config commands ----------------------------------------------------------

int run_config_init(const GlobalFlags& /*flags*/, const std::string& format,
                    const std::string& output_arg, bool force, bool minimal) {
    std::string output = output_arg;
    if (output.empty()) {
        if (format == "kdl") {
            output = ".lci.kdl";
        } else if (format == "yaml") {
            output = ".lci.kdl";
        } else if (format == "json") {
            output = ".lci.kdl.json";
        } else {
            std::cerr << "Error: unsupported format: " << format << "\n";
            return 1;
        }
    }

    if (!force) {
        std::error_code ec;
        if (fs::exists(output, ec)) {
            std::cerr << "Error: configuration file " << output
                      << " already exists (use --force to overwrite)\n";
            return 1;
        }
    }

    std::string content;
    if (format == "kdl") {
        if (minimal) {
            content = R"(// Lightning Code Index Configuration
// Minimal configuration with commonly changed settings

index {
    max_total_size_mb 500          // Total indexed content limit
    max_file_count 10000           // Maximum number of files
    smart_size_control true        // Enable intelligent size management
    priority_mode "recent"         // Priority: "recent", "small", "important"
}

performance {
    max_memory_mb 500              // Memory limit for entire index
}

// Add project-specific exclusions
exclude {
    // "**/my-large-folder/**"
    // "**/*.generated.ts"
}

// Add additional file types to index
include {
    // "*.rs"                      // Rust files
    // "*.zig"                     // Zig files
}
)";
        } else {
            content = R"(// Lightning Code Index Configuration
// Full configuration template with all available options

project {
    name "my-project"
    root "."
}

index {
    max_file_size "10MB"           // Skip files larger than this
    max_total_size_mb 500          // Total indexed content limit
    max_file_count 10000           // Maximum number of files to index
    smart_size_control true        // Enable intelligent size management
    priority_mode "recent"         // Priority: "recent", "small", "important", "balanced"
    follow_symlinks false          // Don't follow symbolic links
}

performance {
    max_memory_mb 500              // Memory limit for entire index
    max_goroutines 8               // Parallel processing limit
    debounce_ms 100                // File change debouncing
}

search {
    max_results 100                // Limit search results
    max_context_lines 50           // Context around matches
    enable_fuzzy true              // Enable fuzzy matching
}

// Include specific file patterns (extends defaults)
include {
    "*.rs"                         // Rust files
    "*.zig"                        // Zig files
    "*.lua"                        // Lua scripts
}

// Exclude specific patterns (extends defaults)
// Note: All hidden directories (.*/) are excluded by default
exclude {
    "**/my-large-data/**"          // Project-specific exclusions
    "**/*.generated.ts"            // Generated TypeScript
}
)";
        }
    } else if (format == "json") {
        nlohmann::json cfg;
        cfg["version"] = 1;
        cfg["project"]["name"] = "my-project";
        cfg["project"]["root"] = ".";
        cfg["index"]["max_file_size"] = 10 * 1024 * 1024;
        cfg["index"]["max_total_size_mb"] = 500;
        cfg["index"]["max_file_count"] = 10000;
        cfg["index"]["follow_symlinks"] = false;
        cfg["index"]["smart_size_control"] = true;
        cfg["index"]["priority_mode"] = "recent";
        cfg["performance"]["max_memory_mb"] = 500;
        cfg["performance"]["max_goroutines"] = 8;
        cfg["performance"]["debounce_ms"] = 100;
        cfg["search"]["max_results"] = 100;
        cfg["search"]["max_context_lines"] = 50;
        cfg["search"]["enable_fuzzy"] = true;
        cfg["include"] = {"*.go", "*.js", "*.jsx", "*.ts", "*.tsx", "*.py"};
        cfg["exclude"] = {"**/.*/**", "**/node_modules/**", "**/vendor/**"};
        content = cfg.dump(2) + "\n";
    } else if (format == "yaml") {
        content = R"(version: 1
project:
  root: "."
  name: "my-project"
index:
  max_file_size: 10485760  # 10MB
  max_total_size_mb: 500
  max_file_count: 10000
  follow_symlinks: false
  smart_size_control: true
  priority_mode: "recent"
performance:
  max_memory_mb: 500
  max_goroutines: 8
  debounce_ms: 100
search:
  max_results: 100
  max_context_lines: 50
  enable_fuzzy: true
include:
  - "*.go"
  - "*.js"
  - "*.jsx"
  - "*.ts"
  - "*.tsx"
  - "*.py"
exclude:
  - "**/.*/**"
  - "**/node_modules/**"
  - "**/vendor/**"
)";
    } else {
        std::cerr << "Error: unsupported format: " << format << "\n";
        return 1;
    }

    std::ofstream ofs(output);
    if (!ofs) {
        std::cerr << "Error: failed to write config file: " << output << "\n";
        return 1;
    }
    ofs << content;
    ofs.close();

    std::printf("Configuration file created: %s\n", output.c_str());
    std::printf("Edit the file to customize settings for your project.\n");

    if (format == "kdl") {
        std::printf("\nCommon customizations:\n");
        std::printf(
            "  - Adjust memory limits: index.max_total_size_mb\n");
        std::printf(
            "  - Add project exclusions: exclude { \"**/my-folder/**\" }\n");
        std::printf(
            "  - Include additional languages: include { \"*.rs\" }\n");
    }

    return 0;
}

int run_config_show(const GlobalFlags& flags, const std::string& format) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }
    if (std::string err = validate_config(cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    if (format == "json") {
        nlohmann::json j;
        j["project"]["name"] = cfg.project.name;
        j["project"]["root"] = cfg.project.root;
        j["index"]["max_file_size"] = cfg.index.max_file_size;
        j["index"]["max_total_size_mb"] = cfg.index.max_total_size_mb;
        j["index"]["max_file_count"] = cfg.index.max_file_count;
        j["index"]["smart_size_control"] = cfg.index.smart_size_control;
        j["index"]["priority_mode"] = cfg.index.priority_mode;
        j["index"]["follow_symlinks"] = cfg.index.follow_symlinks;
        j["index"]["respect_gitignore"] = cfg.index.respect_gitignore;
        j["performance"]["max_memory_mb"] = cfg.performance.max_memory_mb;
        j["performance"]["debounce_ms"] = cfg.performance.debounce_ms;
        j["include"] = cfg.include;
        j["exclude"] = cfg.exclude;
        std::cout << j.dump(2) << "\n";
        return 0;
    }

    // Default: table format
    std::printf("Lightning Code Index Configuration\n");
    std::printf("=================================\n\n");

    std::printf("Project Settings:\n");
    std::printf("  Name:              %s\n", cfg.project.name.c_str());
    std::printf("  Root:              %s\n", cfg.project.root.c_str());
    std::printf("\n");

    std::printf("Index Settings:\n");
    std::printf("  Max file size:     %.1f MB\n",
                static_cast<double>(cfg.index.max_file_size) /
                    (1024.0 * 1024.0));
    std::printf("  Max total size:    %lld MB\n",
                static_cast<long long>(cfg.index.max_total_size_mb));
    std::printf("  Max file count:    %d\n", cfg.index.max_file_count);
    std::printf("  Smart size control: %s\n",
                cfg.index.smart_size_control ? "true" : "false");
    std::printf("  Priority mode:     %s\n",
                cfg.index.priority_mode.c_str());
    std::printf("  Follow symlinks:   %s\n",
                cfg.index.follow_symlinks ? "true" : "false");
    std::printf("  Respect .gitignore: %s\n",
                cfg.index.respect_gitignore ? "true" : "false");
    std::printf("\n");

    std::printf("Performance Settings:\n");
    std::printf("  Max memory:        %d MB\n", cfg.performance.max_memory_mb);
    std::printf("  Max goroutines:    %d\n", cfg.performance.max_goroutines);
    std::printf("  Debounce:          %d ms\n", cfg.performance.debounce_ms);
    std::printf("\n");

    std::printf("Search Settings:\n");
    std::printf("  Max results:       %d\n", cfg.search.max_results);
    std::printf("  Max context lines: %d\n", cfg.search.max_context_lines);
    std::printf("  Enable fuzzy:      %s\n",
                cfg.search.enable_fuzzy ? "true" : "false");
    std::printf("\n");

    std::printf("Include Patterns (%zu):\n", cfg.include.size());
    for (const auto& p : cfg.include) {
        std::printf("  %s\n", p.c_str());
    }
    std::printf("\n");

    std::printf("Exclude Patterns (%zu):\n", cfg.exclude.size());
    for (const auto& p : cfg.exclude) {
        std::printf("  %s\n", p.c_str());
    }

    return 0;
}

int run_config_validate(const GlobalFlags& flags) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::fprintf(stderr, "Configuration validation failed: %s\n",
                     err.c_str());
        return 1;
    }

    std::vector<std::string> warnings;

    if (cfg.performance.max_memory_mb < 100) {
        warnings.push_back(
            "MaxMemoryMB is very low (<100MB), may cause performance issues");
    }
    if (cfg.performance.max_memory_mb > 8000) {
        warnings.push_back(
            "MaxMemoryMB is very high (>8GB), ensure you have sufficient RAM");
    }
    if (cfg.index.max_total_size_mb < 50) {
        warnings.push_back(
            "MaxTotalSizeMB is very low (<50MB), may limit indexing "
            "capability");
    }
    if (cfg.index.max_file_count < 100) {
        warnings.push_back(
            "MaxFileCount is very low (<100), may limit indexing capability");
    }
    if (cfg.include.empty()) {
        warnings.push_back(
            "No include patterns specified, no files will be indexed");
    }

    std::printf("Configuration file is valid\n");
    std::printf("Config source: %s\n", flags.config_path.c_str());
    std::printf("Settings: %d files max, %dMB memory limit, %lldMB index "
                "limit\n",
                cfg.index.max_file_count, cfg.performance.max_memory_mb,
                static_cast<long long>(cfg.index.max_total_size_mb));

    if (!warnings.empty()) {
        std::printf("\nWarnings:\n");
        for (const auto& w : warnings) {
            std::printf("  - %s\n", w.c_str());
        }
    }

    return 0;
}

// -- git-analyze command ------------------------------------------------------

int run_git_analyze(const GlobalFlags& flags, const std::string& scope,
                    const std::string& base_ref, const std::string& target_ref,
                    const std::vector<std::string>& focus, double threshold,
                    int max_findings, bool json_output) {
    if (scope != "staged" && scope != "wip" && scope != "commit" &&
        scope != "range") {
        std::cerr << "Error: invalid scope: " << scope
                  << " (must be staged, wip, commit, or range)\n";
        return 1;
    }

    if (scope == "range" && base_ref.empty()) {
        std::cerr << "Error: --base is required for range scope\n";
        return 1;
    }

    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::string conn_err;
    auto client = ensure_server_running(cfg, conn_err);
    if (!client) {
        std::cerr << "Error: " << conn_err << "\n";
        return 1;
    }

    GitAnalyzeRequest req;
    req.scope = scope;
    req.base_ref = base_ref;
    req.target_ref = target_ref;
    req.focus = focus;
    req.similarity_threshold = threshold;
    req.max_findings = max_findings;

    std::string analyze_err;
    auto result = client->git_analyze(req, analyze_err);
    if (!result) {
        std::cerr << "Error: analysis failed: " << analyze_err << "\n";
        return 1;
    }

    const auto& report =
        (result->contains("report") && (*result)["report"].is_object())
            ? (*result)["report"]
            : *result;

    if (json_output) {
        std::cout << report.dump(2) << "\n";
        return 0;
    }

    // Text output matching Go implementation
    std::printf("Git Change Analysis\n");
    std::printf("==================\n\n");

    if (report.contains("summary")) {
        auto& summary = report["summary"];
        std::printf("Summary\n");
        std::printf("-------\n");
        std::printf("Files changed: %d | Symbols: +%d ~%d\n",
                    summary.value("files_changed", 0),
                    summary.value("symbols_added", 0),
                    summary.value("symbols_modified", 0));
        std::printf("Issues: %d duplicates, %d naming | Risk: %.0f%%\n",
                    summary.value("duplicates_found", 0),
                    summary.value("naming_issues_found", 0),
                    summary.value("risk_score", 0.0) * 100.0);

        if (summary.contains("top_recommendation") &&
            !summary["top_recommendation"].get<std::string>().empty()) {
            std::printf("\nTop recommendation: %s\n",
                        summary["top_recommendation"]
                            .get<std::string>()
                            .c_str());
        }
    }

    if (report.contains("duplicates") && report["duplicates"].is_array()) {
        auto& dups = report["duplicates"];
        if (!dups.empty()) {
            std::printf("\nDuplicates\n");
            std::printf("----------\n");
            for (const auto& dup : dups) {
                std::string severity = dup.value("severity", "");
                for (auto& c : severity) c = static_cast<char>(std::toupper(c));
                std::printf("[%s] %s duplicate (%.0f%%)\n", severity.c_str(),
                            dup.value("type", "").c_str(),
                            dup.value("similarity", 0.0) * 100.0);
                if (dup.contains("new_code")) {
                    std::printf("  New: %s:%d (%s)\n",
                                dup["new_code"].value("file_path", "").c_str(),
                                dup["new_code"].value("start_line", 0),
                                dup["new_code"]
                                    .value("symbol_name", "")
                                    .c_str());
                }
                if (dup.contains("existing_code")) {
                    std::printf(
                        "  Existing: %s:%d (%s)\n",
                        dup["existing_code"].value("file_path", "").c_str(),
                        dup["existing_code"].value("start_line", 0),
                        dup["existing_code"]
                            .value("symbol_name", "")
                            .c_str());
                }
                std::printf("  -> %s\n",
                            dup.value("suggestion", "").c_str());
            }
        }
    }

    if (report.contains("naming_issues") &&
        report["naming_issues"].is_array()) {
        auto& issues = report["naming_issues"];
        if (!issues.empty()) {
            std::printf("\nNaming Issues\n");
            std::printf("-------------\n");
            for (const auto& issue : issues) {
                std::string severity = issue.value("severity", "");
                for (auto& c : severity)
                    c = static_cast<char>(std::toupper(c));
                std::printf("[%s] %s\n", severity.c_str(),
                            issue.value("issue_type", "").c_str());
                if (issue.contains("new_symbol")) {
                    std::printf(
                        "  Symbol: %s (%s:%d)\n",
                        issue["new_symbol"].value("name", "").c_str(),
                        issue["new_symbol"].value("file_path", "").c_str(),
                        issue["new_symbol"].value("line", 0));
                }
                std::printf("  Issue: %s\n",
                            issue.value("issue", "").c_str());
                std::printf("  -> %s\n",
                            issue.value("suggestion", "").c_str());
            }
        }
    }

    if (report.contains("metadata")) {
        auto& meta = report["metadata"];
        std::printf("\nAnalysis: %s -> %s (%dms)\n",
                    meta.value("base_ref", "").c_str(),
                    meta.value("target_ref", "").c_str(),
                    meta.value("analysis_time_ms", 0));
    }

    return 0;
}

// -- symbols command ----------------------------------------------------------

int run_symbols(const GlobalFlags& flags, const std::string& kind,
                bool exported, const std::string& file,
                const std::string& name, const std::string& receiver,
                int min_complexity, int max_complexity,
                const std::string& sort, int max_results, bool json_output) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::string conn_err;
    auto client = ensure_server_running(cfg, conn_err);
    if (!client) {
        std::cerr << "Error: " << conn_err << "\n";
        return 1;
    }

    ListSymbolsRequest req;
    req.kind = kind;
    req.file = file;
    req.name = name;
    req.receiver = receiver;
    req.sort = sort;
    req.max = max_results;
    if (exported) {
        req.exported = true;
    }
    if (min_complexity > 0) {
        req.min_complexity = min_complexity;
    }
    if (max_complexity > 0) {
        req.max_complexity = max_complexity;
    }

    std::string sym_err;
    auto result = client->list_symbols(req, sym_err);
    if (!result) {
        std::cerr << "Error: list symbols failed: " << sym_err << "\n";
        return 1;
    }

    if (json_output) {
        std::cout << result->dump(2) << "\n";
        return 0;
    }

    // Text output matching Go implementation
    if (result->contains("symbols") && (*result)["symbols"].is_array()) {
        for (const auto& sym : (*result)["symbols"]) {
            std::string sig = sym.value("signature", "");
            if (sig.empty()) {
                sig = sym.value("name", "");
            }
            std::string exp_str;
            if (sym.value("is_exported", false)) {
                exp_str = " [exported]";
            }
            std::string comp_str;
            int comp = sym.value("complexity", 0);
            if (comp > 0) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), " (complexity:%d)", comp);
                comp_str = buf;
            }
            std::printf("%s:%d: %s %s%s%s\n",
                        sym.value("file", "").c_str(),
                        sym.value("line", 0),
                        sym.value("type", "").c_str(), sig.c_str(),
                        exp_str.c_str(), comp_str.c_str());
        }
    }

    bool has_more = result->value("has_more", false);
    if (has_more) {
        int showing = result->value("showing", 0);
        int total = result->value("total", 0);
        std::fprintf(stderr, "\n(%d of %d shown, use --max to see more)\n",
                     showing, total);
    }

    return 0;
}

// -- inspect command ----------------------------------------------------------

int run_inspect(const GlobalFlags& flags, const std::string& name,
                const std::string& type, const std::string& file,
                const std::string& include_sections, bool json_output) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::string conn_err;
    auto client = ensure_server_running(cfg, conn_err);
    if (!client) {
        std::cerr << "Error: " << conn_err << "\n";
        return 1;
    }

    InspectSymbolRequest req;
    req.name = name;
    req.type = type;
    req.file = file;
    req.include = include_sections.empty() ? "signature" : include_sections;

    std::string insp_err;
    auto result = client->inspect_symbol(req, insp_err);
    if (!result) {
        std::cerr << "Error: inspect failed: " << insp_err << "\n";
        return 1;
    }

    if (json_output) {
        std::cout << result->dump(2) << "\n";
        return 0;
    }

    // Text output matching Go implementation
    if (result->contains("symbols") && (*result)["symbols"].is_array()) {
        int idx = 0;
        for (const auto& sym : (*result)["symbols"]) {
            if (idx > 0) {
                std::printf("---\n");
            }
            std::printf("%s (%s) %s:%d\n", sym.value("name", "").c_str(),
                        sym.value("type", "").c_str(),
                        sym.value("file", "").c_str(),
                        sym.value("line", 0));

            std::string sig = sym.value("signature", "");
            if (!sig.empty()) {
                std::printf("  Signature: %s\n", sig.c_str());
            }
            std::string doc = sym.value("doc_comment", "");
            if (!doc.empty()) {
                std::printf("  Doc: %s\n", doc.c_str());
            }
            int comp = sym.value("complexity", 0);
            if (comp > 0) {
                std::printf("  Complexity: %d\n", comp);
            }
            std::string recv = sym.value("receiver_type", "");
            if (!recv.empty()) {
                std::printf("  Receiver: %s\n", recv.c_str());
            }
            if (sym.contains("callers") && sym["callers"].is_array() &&
                !sym["callers"].empty()) {
                std::printf("  Callers: ");
                bool first = true;
                for (const auto& c : sym["callers"]) {
                    if (!first) std::printf(", ");
                    std::printf("%s", c.get<std::string>().c_str());
                    first = false;
                }
                std::printf("\n");
            }
            if (sym.contains("callees") && sym["callees"].is_array() &&
                !sym["callees"].empty()) {
                std::printf("  Callees: ");
                bool first = true;
                for (const auto& c : sym["callees"]) {
                    if (!first) std::printf(", ");
                    std::printf("%s", c.get<std::string>().c_str());
                    first = false;
                }
                std::printf("\n");
            }
            if (sym.contains("type_hierarchy") &&
                !sym["type_hierarchy"].is_null()) {
                auto& th = sym["type_hierarchy"];
                if (th.contains("implements") && !th["implements"].empty()) {
                    std::printf("  Implements: ");
                    bool first = true;
                    for (const auto& v : th["implements"]) {
                        if (!first) std::printf(", ");
                        std::printf("%s", v.get<std::string>().c_str());
                        first = false;
                    }
                    std::printf("\n");
                }
                if (th.contains("implemented_by") &&
                    !th["implemented_by"].empty()) {
                    std::printf("  Implemented by: ");
                    bool first = true;
                    for (const auto& v : th["implemented_by"]) {
                        if (!first) std::printf(", ");
                        std::printf("%s", v.get<std::string>().c_str());
                        first = false;
                    }
                    std::printf("\n");
                }
            }
            if (sym.contains("scope_chain") && sym["scope_chain"].is_array() &&
                !sym["scope_chain"].empty()) {
                std::printf("  Scope: ");
                bool first = true;
                for (const auto& s : sym["scope_chain"]) {
                    if (!first) std::printf(" > ");
                    std::printf("%s", s.get<std::string>().c_str());
                    first = false;
                }
                std::printf("\n");
            }
            if (sym.contains("annotations") &&
                sym["annotations"].is_array() &&
                !sym["annotations"].empty()) {
                std::printf("  Annotations: ");
                bool first = true;
                for (const auto& a : sym["annotations"]) {
                    if (!first) std::printf(", ");
                    std::printf("%s", a.get<std::string>().c_str());
                    first = false;
                }
                std::printf("\n");
            }
            std::printf("  Refs: %d incoming, %d outgoing\n",
                        sym.value("incoming_refs", 0),
                        sym.value("outgoing_refs", 0));
            ++idx;
        }
    }

    return 0;
}

// -- browse command -----------------------------------------------------------

int run_browse(const GlobalFlags& flags, const std::string& file_path,
               const std::string& kind, bool exported,
               const std::string& sort, bool show_imports, bool show_stats,
               bool json_output) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::string conn_err;
    auto client = ensure_server_running(cfg, conn_err);
    if (!client) {
        std::cerr << "Error: " << conn_err << "\n";
        return 1;
    }

    BrowseFileRequest req;
    req.file = file_path;
    req.kind = kind;
    req.sort = sort;
    req.show_imports = show_imports;
    // Go's browse surface accepts --stats but does not currently enrich the
    // CLI/JSON payload with a dedicated stats block for this command path.
    // Keep C++ aligned with that contract instead of emitting extra fields.
    req.show_stats = false;
    if (exported) {
        req.exported = true;
    }

    std::string browse_err;
    auto result = client->browse_file(req, browse_err);
    if (!result) {
        std::cerr << "Error: browse failed: " << browse_err << "\n";
        return 1;
    }

    if (json_output) {
        std::cout << result->dump(2) << "\n";
        return 0;
    }

    // Text output matching Go implementation
    if (result->contains("file")) {
        auto& fi = (*result)["file"];
        std::printf("File: %s", fi.value("path", "").c_str());
        std::string lang = fi.value("language", "");
        if (!lang.empty()) {
            std::printf(" (%s)", lang.c_str());
        }
        std::printf("\n");
    }

    if (result->contains("stats") && !(*result)["stats"].is_null()) {
        auto& st = (*result)["stats"];
        std::printf("Stats: %d symbols (%d functions, %d types, %d exported)",
                    st.value("symbol_count", 0),
                    st.value("function_count", 0), st.value("type_count", 0),
                    st.value("exported_count", 0));
        double avg_comp = st.value("avg_complexity", 0.0);
        if (avg_comp > 0) {
            std::printf(", avg complexity: %.1f, max: %d", avg_comp,
                        st.value("max_complexity", 0));
        }
        std::printf("\n");
    }

    if (result->contains("imports") && (*result)["imports"].is_array() &&
        !(*result)["imports"].empty()) {
        std::printf("\nImports:\n");
        for (const auto& imp : (*result)["imports"]) {
            std::printf("  %s\n", imp.get<std::string>().c_str());
        }
    }

    if (result->contains("symbols") && (*result)["symbols"].is_array()) {
        int total = result->value("total", 0);
        std::printf("\nSymbols (%d):\n", total);
        for (const auto& sym : (*result)["symbols"]) {
            std::string sig = sym.value("signature", "");
            if (sig.empty()) {
                sig = sym.value("name", "");
            }
            std::string exp_str;
            if (sym.value("is_exported", false)) {
                exp_str = " [exported]";
            }
            std::printf("  %4d: %-10s %s%s\n", sym.value("line", 0),
                        sym.value("type", "").c_str(), sig.c_str(),
                        exp_str.c_str());
        }
    }

    return 0;
}

}  // namespace cli
}  // namespace lci
