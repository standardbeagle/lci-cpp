#include <lci/cli/commands.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

namespace lci {
namespace cli {

int run_status(const GlobalFlags& flags, bool json_output, bool verbose) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::string conn_err;
    auto client = ensure_server_running(cfg, conn_err);
    if (!client) {
        std::cerr << "Error: failed to connect to server: " << conn_err
                  << "\n";
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

    if (json_output) {
        nlohmann::json report;
        report["ready"] = status->ready;
        report["file_count"] = stats->file_count;
        report["symbol_count"] = stats->symbol_count;
        report["index_size_bytes"] = stats->index_size_bytes;
        report["build_duration_ms"] = stats->build_duration_ms;
        report["memory_rss_mb"] = stats->memory_rss_mb;
        report["num_threads"] = stats->num_threads;
        report["uptime_seconds"] = stats->uptime_seconds;
        report["search_count"] = stats->search_count;
        report["avg_search_time_ms"] = stats->avg_search_time_ms;
        std::cout << report.dump(2) << "\n";
        return 0;
    }

    std::printf("Lightning Code Index Server Status\n");
    std::printf("==================================\n\n");

    if (status->ready) {
        std::printf("Status: Ready\n");
    } else if (status->indexing_active) {
        std::printf("Status: Indexing (%.1f%% complete)\n",
                    status->progress * 100.0);
    } else {
        std::printf("Status: Not Ready\n");
    }

    std::printf("\nIndex Statistics:\n");
    std::printf("  Files indexed:    %d\n", stats->file_count);
    std::printf("  Symbols indexed:  %d\n", stats->symbol_count);
    std::printf("  Index size:       %s\n",
                format_bytes(stats->index_size_bytes).c_str());
    std::printf("  Build time:       %s\n",
                format_milliseconds(stats->build_duration_ms).c_str());

    std::printf("\nServer Runtime:\n");
    std::printf("  Uptime:           %s\n",
                format_seconds(stats->uptime_seconds).c_str());
    std::printf("  Threads:          %d\n", stats->num_threads);

    std::printf("\nMemory Usage:\n");
    std::printf("  RSS:              %.1f MB\n", stats->memory_rss_mb);

    if (stats->search_count > 0 || verbose) {
        std::printf("\nSearch Statistics:\n");
        std::printf("  Total searches:   %lld\n",
                    static_cast<long long>(stats->search_count));
        if (stats->search_count > 0) {
            std::printf("  Avg search time:  %.2f ms\n",
                        stats->avg_search_time_ms);
        }
    }

    if (verbose) {
        std::printf("\nDetailed Information:\n");
        std::printf("  Index size (bytes):     %lld\n",
                    static_cast<long long>(stats->index_size_bytes));
        std::printf("  Build duration (ms):    %lld\n",
                    static_cast<long long>(stats->build_duration_ms));
        std::printf("  Uptime (seconds):       %.2f\n",
                    stats->uptime_seconds);
    }

    return 0;
}

}  // namespace cli
}  // namespace lci
