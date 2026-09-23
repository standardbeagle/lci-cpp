#include <lci/cli/commands.h>

#include "name_aggregation.h"
#include "commands_shared.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>

#include <lci/indexing/pipeline_scanner.h>
#include <lci/semantic/fuzzy_matcher.h>
#include <nlohmann/json.hpp>

#include "ast_filters.h"
#include "symbol_filters.h"
#include "tree_formatter.h"

namespace lci {
namespace cli {

namespace fs = std::filesystem;

// -- config commands ----------------------------------------------------------

int run_config_init(const GlobalFlags& /*flags*/, const std::string& format,
                    const std::string& output_arg, bool force, bool minimal) {
    // YAML is write-only: nothing in this binary can read it back. The old
    // code wrote a YAML document into ".lci.kdl", so the very next `lci` run
    // failed to parse the config it had just been told to generate. Refuse
    // instead of producing a file that cannot work.
    if (format == "yaml") {
        std::cerr << "Error: yaml config is not supported — lci only reads "
                     "KDL. Use -f kdl.\n";
        return 1;
    }

    std::string output = output_arg;
    if (output.empty()) {
        if (format == "kdl") {
            output = ".lci.kdl";
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
    max_total_size_mb 500          // Corpus budget, spent in priority order
    max_file_count 50000           // Corpus budget: max files indexed
    overflow_policy "reduced"      // Past budget: "reduced" (partial, warned) or "reject"
    data_file_token_cap 4096       // Unique search tokens kept per DATA file (json/csv/txt);
                                   // capped files still match every search (0 = uncapped)
    smart_size_control true        // Enable intelligent size management
    priority_mode "recent"         // Priority: "recent", "small", "important"
}

performance {
    max_memory_mb 500              // File-content cache cap (LRU-evicted)
}

// Add project-specific exclusions
exclude {
    // "**/my-large-folder/**"
    // "**/*.generated.ts"
}

// Add additional file types to index. An include section must carry at
// least one pattern (a present-but-empty section is a config error), so
// the whole block stays commented until you need it:
// include {
//     "*.rs"                      // Rust files
//     "*.zig"                     // Zig files
// }
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
    max_total_size_mb 500          // Corpus budget, spent in priority order
    max_file_count 50000           // Corpus budget: max files indexed
    overflow_policy "reduced"      // Past budget: "reduced" (partial, warned) or "reject"
    data_file_token_cap 4096       // Unique search tokens kept per DATA file (json/csv/txt);
                                   // capped files still match every search (0 = uncapped)
    smart_size_control true        // Enable intelligent size management
    priority_mode "recent"         // Priority: "recent", "small", "important", "balanced"
    follow_symlinks false          // Don't follow symbolic links
}

performance {
    max_memory_mb 500              // File-content cache cap (LRU-evicted)
    max_goroutines 8               // Parallel processing limit
    debounce_ms 100                // File change debouncing
}

search {
    max_results 100                // Limit search results
    max_context_lines 50           // Context around matches
    enable_fuzzy true              // Enable fuzzy matching
}

// Frameworks such as React, Preact, Solid, Qwik, Vue, and Svelte use
// PascalCase noun names for function components. Replace this list when a
// project uses that convention in other file types.
naming {
    component_function_extensions ".js" ".jsx" ".mjs" ".cjs" ".tsx" ".vue" ".svelte"
}

// Include specific file patterns. An include section REPLACES the
// built-in defaults, so uncomment it only to narrow indexing to exactly
// these patterns (once present, it must name at least one pattern).
// include {
//     "*.rs"                         // Rust files
//     "*.zig"                        // Zig files
// }

// Exclude specific patterns. An exclude section REPLACES the ~110 built-in
// default excludes (node_modules, build output, ...), so copy the defaults
// from `lci config show` into it before adding project paths.
// Note: all hidden directories (.*/) are skipped regardless of this list.
// exclude {
//     "**/my-large-data/**"          // Project-specific exclusions
//     "**/*.generated.ts"            // Generated TypeScript
// }
)";
        }
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
            "  - Replace the exclude list: exclude { \"**/my-folder/**\" }\n"
            "    (replaces the built-in excludes; see `lci config show`)\n");
        std::printf(
            "  - Restrict indexing to patterns: include { \"*.rs\" }\n"
            "    (replaces the built-in include set)\n");
    }

    return 0;
}

int run_config_show(const GlobalFlags& flags, const std::string& format) {
    if (format != "table" && format != "json") {
        std::cerr << "Error: unsupported format: " << format
                  << " (supported: table, json)\n";
        return 1;
    }
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
        // The full effective config: this used to omit server, insight,
        // synonyms, attributes, and three index budget fields, so `config
        // show -f json` could not answer "what will actually run" for
        // anything past the original 13 keys.
        nlohmann::json j;
        j["version"] = cfg.version;
        j["project"]["name"] = cfg.project.name;
        j["project"]["root"] = cfg.project.root;
        j["index"]["max_file_size"] = cfg.index.max_file_size;
        j["index"]["max_parse_file_size"] = cfg.index.max_parse_file_size;
        j["index"]["data_file_token_cap"] = cfg.index.data_file_token_cap;
        j["index"]["max_total_size_mb"] = cfg.index.max_total_size_mb;
        j["index"]["max_file_count"] = cfg.index.max_file_count;
        j["index"]["overflow_policy"] = cfg.index.overflow_policy;
        j["index"]["smart_size_control"] = cfg.index.smart_size_control;
        j["index"]["priority_mode"] = cfg.index.priority_mode;
        j["index"]["follow_symlinks"] = cfg.index.follow_symlinks;
        j["index"]["respect_gitignore"] = cfg.index.respect_gitignore;
        j["index"]["watch_mode"] = cfg.index.watch_mode;
        j["index"]["watch_debounce_ms"] = cfg.index.watch_debounce_ms;
        j["performance"]["max_memory_mb"] = cfg.performance.max_memory_mb;
        j["performance"]["max_goroutines"] = cfg.performance.max_goroutines;
        j["performance"]["debounce_ms"] = cfg.performance.debounce_ms;
        j["performance"]["parallel_file_workers"] =
            cfg.performance.parallel_file_workers;
        j["performance"]["indexing_timeout_sec"] =
            cfg.performance.indexing_timeout_sec;
        j["server"]["idle_timeout_sec"] = cfg.server.idle_timeout_sec;
        j["server"]["max_instances"] = cfg.server.max_instances;
        j["server"]["max_rss_mb"] = cfg.server.max_rss_mb;
        j["insight"]["error_report"] = cfg.insight.error_report;
        j["insight"]["entry_points"] = cfg.insight.entry_points;
        j["naming"]["component_function_extensions"] =
            cfg.naming.component_function_extensions;
        j["search"]["default_context_lines"] =
            cfg.search.default_context_lines;
        j["search"]["max_results"] = cfg.search.max_results;
        j["search"]["max_context_lines"] = cfg.search.max_context_lines;
        j["search"]["enable_fuzzy"] = cfg.search.enable_fuzzy;
        j["search"]["merge_file_results"] = cfg.search.merge_file_results;
        j["search"]["ensure_complete_stmt"] = cfg.search.ensure_complete_stmt;
        j["include"] = cfg.include;
        j["exclude"] = cfg.exclude;
        // SynonymTable exposes no group-listing accessor (only lookups and
        // a count) — report the count rather than skip the section.
        j["synonyms"]["group_count"] = cfg.synonyms.group_count();
        for (const auto& def : cfg.attribute_defs) {
            nlohmann::json d;
            d["name"] = def.name;
            d["rank"] = def.rank;
            d["dirs"] = def.dirs;
            d["globs"] = def.globs;
            d["contents"] = def.contents;
            j["attributes"]["definitions"].push_back(std::move(d));
        }
        for (const auto& rule : cfg.attributes) {
            nlohmann::json r;
            r["attr"] = rule.attr;
            r["pattern"] = rule.pattern;
            j["attributes"]["patterns"].push_back(std::move(r));
        }
        if (!j.contains("attributes")) {
            j["attributes"] = nlohmann::json::object();
        }
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
    std::printf("  Overflow policy:   %s\n",
                cfg.index.overflow_policy.c_str());
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

    std::printf("Naming Settings:\n");
    std::printf("  Component-function extensions (%zu):\n",
                cfg.naming.component_function_extensions.size());
    for (const auto& extension : cfg.naming.component_function_extensions) {
        std::printf("    %s\n", extension.c_str());
    }
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
    std::string source;
    if (std::string err = load_config_with_overrides(flags, cfg, &source);
        !err.empty()) {
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
    // No warning for an empty cfg.include: with no include section the
    // scanner indexes every supported file type (the defaults), and a
    // present-but-empty include section never reaches here — config
    // loading rejects it as an error. The old "no files will be indexed"
    // warning described behavior the server never had.

    std::printf("Configuration file is valid\n");
    std::string display_source = source;
    if (!source.empty()) {
        // ConfigResult::source is an absolute path (config.cpp resolves
        // <root>/.lci.kdl before returning it). Show it relative to the
        // cwd when possible so the output stays stable across checkouts
        // instead of leaking the invoking machine's absolute path.
        std::error_code rel_ec;
        auto rel = fs::relative(source, fs::current_path(), rel_ec);
        if (!rel_ec && !rel.empty()) {
            display_source = rel.string();
        }
    }
    std::printf(
        "Config source: %s\n",
        display_source.empty() ? "(defaults, no config file)"
                               : display_source.c_str());
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

}  // namespace cli
}  // namespace lci
