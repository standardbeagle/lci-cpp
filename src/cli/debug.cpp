#include <lci/cli/commands.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>
#include <lci/server/server.h>

// S9: every `lci debug` number comes from the running index server's own
// /status and /stats endpoints. The previous implementation ran a second,
// private extractor (the old src/symbol-linker tree, since deleted) and printed hardcoded
// zeros as measurements ("Total Symbols: 0", "0 files processed") — fabricated
// data, banned by karpathy-principles rule 6. A subcommand that cannot reach
// a server exits non-zero with "server not running" rather than printing
// invented numbers.

namespace lci {
namespace cli {

namespace {

struct ServerSnapshot {
    IndexStatus status;
    StatsResponse stats;
    bool stats_available = false;
};

// Connects to an already-running server WITHOUT spawning one (debug/status
// are queries, not supervisors). Prints "server not running" and returns
// false when no server answers.
bool query_server(Client& client, ServerSnapshot& out) {
    if (!client.is_server_running()) {
        std::printf("server not running\n");
        return false;
    }
    std::string err;
    auto status = client.get_status(err);
    if (!status) {
        std::fprintf(stderr, "Error: failed to get server status: %s\n",
                     err.c_str());
        return false;
    }
    out.status = *status;
    // /stats refuses until the index is ready; absence is reported, never
    // faked with zeros.
    err.clear();
    if (auto stats = client.get_stats(err)) {
        out.stats = *stats;
        out.stats_available = true;
    }
    return true;
}

std::filesystem::path display_root(const Config& cfg) {
    if (!cfg.project.root.empty()) return cfg.project.root;
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path(".") : cwd;
}

int debug_removed_linker_command(const char* name) {
    std::printf(
        "debug %s: unavailable — the symbol-linker dependency engine was "
        "removed (S9); it was a second, stale extractor reachable only from "
        "this command\n",
        name);
    return 1;
}

}  // namespace

// -- debug info ---------------------------------------------------------------

int run_debug_info(const GlobalFlags& flags, bool verbose, bool incremental) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }
    if (incremental) {
        std::fprintf(stderr,
                     "Error: --incremental is not supported: debug info "
                     "reports the running server's /status numbers only\n");
        return 1;
    }

    const std::string socket_path =
        get_socket_path_for_root(display_root(cfg).string());
    Client client(socket_path);
    ServerSnapshot snap;
    if (!query_server(client, snap)) {
        return 1;
    }

    std::printf("Debug Info - Lightning Code Index\n");
    std::printf("Root Path: %s\n", display_root(cfg).string().c_str());
    std::printf("Server: %s\n", socket_path.c_str());
    std::printf("\n");
    if (snap.status.ready) {
        std::printf("Status: ready\n");
    } else if (snap.status.indexing_active) {
        std::printf("Status: indexing (%d%% complete, phase %s)\n",
                    snap.status.percent_complete, snap.status.phase.c_str());
    } else {
        std::printf("Status: not ready\n");
    }
    std::printf("Files indexed: %d\n", snap.status.file_count);
    std::printf("Symbols indexed: %d\n", snap.status.symbol_count);
    if (snap.stats_available) {
        std::printf("Index size: %lld bytes\n",
                    static_cast<long long>(snap.stats.index_size_bytes));
        std::printf("Build duration: %lld ms\n",
                    static_cast<long long>(snap.stats.build_duration_ms));
        std::printf("Uptime: %.3f s\n", snap.stats.uptime_seconds);
        std::printf("Threads: %d\n", snap.stats.num_threads);
        std::printf("RSS: %.1f MB\n", snap.stats.memory_rss_mb);
        std::printf("Searches: %lld\n",
                    static_cast<long long>(snap.stats.search_count));
        std::printf("Avg search time: %.2f ms\n",
                    snap.stats.avg_search_time_ms);
    } else {
        std::printf("Stats: unavailable (index not ready)\n");
    }
    if (verbose) {
        // Verbose: dump the raw /status payload the numbers came from.
        std::printf("\nRaw /status:\n");
        nlohmann::json j;
        j["ready"] = snap.status.ready;
        j["file_count"] = snap.status.file_count;
        j["symbol_count"] = snap.status.symbol_count;
        j["indexing_active"] = snap.status.indexing_active;
        j["files_scanned"] = snap.status.files_scanned;
        j["percent_complete"] = snap.status.percent_complete;
        j["phase"] = snap.status.phase;
        std::printf("%s\n", j.dump(2).c_str());
    }
    return 0;
}

// -- debug validate -----------------------------------------------------------

int run_debug_validate(const GlobalFlags& flags, bool incremental) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    const std::string socket_path =
        get_socket_path_for_root(display_root(cfg).string());
    Client client(socket_path);

    std::printf("Validating index server\n");
    std::printf("Root Path: %s\n", cfg.project.root.c_str());
    if (incremental) {
        std::fprintf(stderr,
                     "Note: --incremental: server-side consistency check "
                     "is mode-agnostic in C++ port; output is identical to "
                     "full-mode for now.\n");
    }
    std::printf("\n");

    if (!client.is_server_running()) {
        std::printf("server not running\n");
        return 1;
    }
    std::string status_err;
    auto status = client.get_status(status_err);
    if (!status) {
        std::fprintf(stderr, "Error: failed to get server status: %s\n",
                     status_err.c_str());
        return 1;
    }

    std::printf("Running consistency checks...\n");
    if (status->error.empty()) {
        std::printf("All consistency checks passed!\n");
        return 0;
    }

    std::printf("Found consistency issue: %s\n", status->error.c_str());
    std::printf(
        "\nRecommendation: Review the issues above and check your "
        "configuration.\n");
    return 1;
}

// -- debug deps / graph -------------------------------------------------------
//
// The dependency graph was computed by the old symbol-linker tree, deleted in S9 (its
// resolution was wrong: Go imports always external, PHP namespaces mapped to
// directories, SymbolID collisions past 10k symbols). The subcommand
// registrations live in cli/main.cpp (S12 scope); until S12 removes them the
// entry points fail fast instead of fabricating a graph.

int run_debug_deps(const GlobalFlags& /*flags*/, bool /*verbose*/) {
    return debug_removed_linker_command("deps");
}

int run_debug_graph(const GlobalFlags& /*flags*/, const std::string& /*output*/) {
    return debug_removed_linker_command("graph");
}

// -- debug export -------------------------------------------------------------

int run_debug_export(const GlobalFlags& flags, const std::string& output,
                     bool verbose, bool incremental) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }
    if (incremental) {
        std::fprintf(stderr,
                     "Error: --incremental is not supported: export writes "
                     "the running server's /status + /stats numbers only\n");
        return 1;
    }

    std::printf("Exporting Debug Information\n");
    std::printf("Root Path: %s\n", cfg.project.root.c_str());
    std::printf("Output File: %s\n", output.c_str());
    std::printf("\n");

    const std::string socket_path =
        get_socket_path_for_root(display_root(cfg).string());
    Client client(socket_path);
    ServerSnapshot snap;
    if (!query_server(client, snap)) {
        return 1;
    }
    if (!snap.stats_available) {
        std::fprintf(stderr,
                     "Error: server index not ready; /stats unavailable\n");
        return 1;
    }

    nlohmann::json data;
    data["root"] = display_root(cfg).string();
    data["ready"] = snap.status.ready;
    data["file_count"] = snap.stats.file_count;
    data["symbol_count"] = snap.stats.symbol_count;
    data["index_size_bytes"] = snap.stats.index_size_bytes;
    data["build_duration_ms"] = snap.stats.build_duration_ms;
    data["memory_rss_mb"] = snap.stats.memory_rss_mb;
    data["uptime_seconds"] = snap.stats.uptime_seconds;
    data["search_count"] = snap.stats.search_count;
    data["avg_search_time_ms"] = snap.stats.avg_search_time_ms;

    const std::string json_str = data.dump(2);

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

}  // namespace cli
}  // namespace lci
