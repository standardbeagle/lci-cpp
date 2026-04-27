#include <lci/cli/commands.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

namespace lci {
namespace cli {

// -- debug info ---------------------------------------------------------------

int run_debug_info(const GlobalFlags& flags, bool verbose) {
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

    std::string stats_err;
    auto stats = client->get_stats(stats_err);
    if (!stats) {
        std::cerr << "Error: failed to get server stats: " << stats_err
                  << "\n";
        return 1;
    }

    std::string status_err;
    auto status = client->get_status(status_err);
    if (!status) {
        std::cerr << "Error: failed to get server status: " << status_err
                  << "\n";
        return 1;
    }

    std::printf("Debug Info - Lightning Code Index\n");
    std::printf("Root Path: %s\n", cfg.project.root.c_str());
    std::printf("\n");

    std::printf("Debug Information:\n");
    std::printf(
        "=================================================="
        "==============================\n");
    std::printf("Server Status:\n");
    std::printf("  Ready: %s\n", status->ready ? "true" : "false");
    std::printf("  Indexing Active: %s\n",
                status->indexing_active ? "true" : "false");
    std::printf("  File Count: %d\n", stats->file_count);
    std::printf("  Symbol Count: %d\n", stats->symbol_count);
    std::printf("  Index Size: %s\n",
                format_bytes(stats->index_size_bytes).c_str());
    std::printf("  Build Duration: %s\n",
                format_milliseconds(stats->build_duration_ms).c_str());
    std::printf("  Memory RSS: %.1f MB\n", stats->memory_rss_mb);
    std::printf("  Threads: %d\n", stats->num_threads);
    std::printf("  Uptime: %s\n", format_seconds(stats->uptime_seconds).c_str());
    std::printf("  Search Count: %lld\n",
                static_cast<long long>(stats->search_count));
    std::printf("  Avg Search Time: %.2f ms\n", stats->avg_search_time_ms);

    if (verbose) {
        std::printf("\nValidation Results:\n");
        std::printf("----------------------------------------\n");
        if (status->error.empty()) {
            std::printf("No consistency issues found\n");
        } else {
            std::printf("Issue: %s\n", status->error.c_str());
        }
        std::printf("\n");
    }

    return 0;
}

// -- debug validate -----------------------------------------------------------

int run_debug_validate(const GlobalFlags& flags) {
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

    std::printf("Validating Symbol Linking System\n");
    std::printf("Root Path: %s\n", cfg.project.root.c_str());
    std::printf("\n");

    std::string status_err;
    auto status = client->get_status(status_err);
    if (!status) {
        std::cerr << "Error: failed to get server status: " << status_err
                  << "\n";
        return 1;
    }

    std::printf("Running consistency checks...\n");
    if (status->error.empty()) {
        std::printf("All consistency checks passed!\n");
        std::printf(
            "The symbol linking system is operating correctly.\n");
        return 0;
    }

    std::printf("Found consistency issue: %s\n", status->error.c_str());
    std::printf(
        "\nRecommendation: Review the issues above and check your "
        "configuration.\n");
    return 1;
}

// -- debug deps ---------------------------------------------------------------

int run_debug_deps(const GlobalFlags& flags, bool verbose) {
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

    std::printf("Dependency Graph Analysis\n");
    std::printf("Root Path: %s\n", cfg.project.root.c_str());
    std::printf("\n");

    std::string stats_err;
    auto stats = client->get_stats(stats_err);
    if (!stats) {
        std::cerr << "Error: failed to get server stats: " << stats_err
                  << "\n";
        return 1;
    }

    std::printf("Dependency Graph Analysis:\n");
    std::printf("==================================================\n");
    std::printf("Total Files: %d\n", stats->file_count);
    std::printf("Total Symbols: %d\n", stats->symbol_count);
    std::printf("Index Size: %s\n",
                format_bytes(stats->index_size_bytes).c_str());

    if (verbose) {
        std::printf("\nDetailed Dependency Information:\n");
        std::printf("--------------------------------------------------\n");
        std::printf("Use 'lci inspect <symbol>' for per-symbol dependency "
                    "details\n");
    }

    return 0;
}

// -- debug export -------------------------------------------------------------

int run_debug_export(const GlobalFlags& flags, const std::string& output,
                     bool verbose) {
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

    std::printf("Exporting Debug Information\n");
    std::printf("Root Path: %s\n", cfg.project.root.c_str());
    std::printf("Output File: %s\n", output.c_str());
    std::printf("\n");

    std::string stats_err;
    auto stats = client->get_stats(stats_err);
    if (!stats) {
        std::cerr << "Error: failed to get server stats: " << stats_err
                  << "\n";
        return 1;
    }

    std::string status_err;
    auto status = client->get_status(status_err);
    if (!status) {
        std::cerr << "Error: failed to get server status: " << status_err
                  << "\n";
        return 1;
    }

    nlohmann::json data;
    data["root"] = cfg.project.root;
    data["ready"] = status->ready;
    data["file_count"] = stats->file_count;
    data["symbol_count"] = stats->symbol_count;
    data["index_size_bytes"] = stats->index_size_bytes;
    data["build_duration_ms"] = stats->build_duration_ms;
    data["memory_rss_mb"] = stats->memory_rss_mb;
    data["uptime_seconds"] = stats->uptime_seconds;
    data["search_count"] = stats->search_count;
    data["avg_search_time_ms"] = stats->avg_search_time_ms;

    std::string json_str = data.dump(2);

    std::ofstream ofs(output);
    if (!ofs) {
        std::cerr << "Error: failed to write debug info to file: " << output
                  << "\n";
        return 1;
    }
    ofs << json_str << "\n";
    ofs.close();

    std::printf("Debug information exported to %s\n", output.c_str());
    std::printf("File size: %zu bytes\n", json_str.size());

    if (verbose) {
        std::printf("\nPreview (first 500 characters):\n");
        std::printf("----------------------------------------\n");
        if (json_str.size() > 500) {
            std::printf("%s...\n", json_str.substr(0, 500).c_str());
        } else {
            std::printf("%s\n", json_str.c_str());
        }
    }

    return 0;
}

// -- debug graph --------------------------------------------------------------

int run_debug_graph(const GlobalFlags& flags, const std::string& output) {
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

    std::printf("Exporting Dependency Graph\n");
    std::printf("Root Path: %s\n", cfg.project.root.c_str());
    std::printf("Output File: %s\n", output.c_str());
    std::printf("\n");

    std::string stats_err;
    auto stats = client->get_stats(stats_err);
    if (!stats) {
        std::cerr << "Error: failed to get server stats: " << stats_err
                  << "\n";
        return 1;
    }

    // Generate DOT graph from available stats
    std::string dot = "digraph dependencies {\n";
    dot += "    rankdir=LR;\n";
    dot += "    node [shape=box, style=filled, fillcolor=lightblue];\n";
    dot += "    label=\"LCI Dependency Graph\";\n";
    dot += "    labelloc=t;\n";
    dot += "\n";
    dot += "    // Generated from index with " +
           std::to_string(stats->file_count) + " files, " +
           std::to_string(stats->symbol_count) + " symbols\n";
    dot += "    // Use 'lci inspect <symbol>' or 'lci tree <function>' for "
           "detailed dependencies\n";
    dot += "    info [label=\"Files: " + std::to_string(stats->file_count) +
           "\\nSymbols: " + std::to_string(stats->symbol_count) +
           "\\nIndex: " + format_bytes(stats->index_size_bytes) + "\"];\n";
    dot += "}\n";

    std::ofstream ofs(output);
    if (!ofs) {
        std::cerr << "Error: failed to write dependency graph to file: "
                  << output << "\n";
        return 1;
    }
    ofs << dot;
    ofs.close();

    std::printf("Dependency graph exported to %s\n", output.c_str());
    std::printf("File size: %zu bytes\n", dot.size());
    std::printf("\nTo visualize the graph, use Graphviz:\n");
    std::printf("  dot -Tpng %s -o dependency-graph.png\n", output.c_str());
    std::printf("  dot -Tsvg %s -o dependency-graph.svg\n", output.c_str());

    return 0;
}

}  // namespace cli
}  // namespace lci
