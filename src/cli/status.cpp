#include <lci/cli/commands.h>

#include <chrono>
#include <ctime>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace lci {
namespace cli {

namespace {

std::string iso_timestamp_now() {
    auto now = std::chrono::system_clock::now();
    auto now_time = std::chrono::system_clock::to_time_t(now);
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                      now.time_since_epoch())
                      .count() %
                  1000000;

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now_time);
#else
    localtime_r(&now_time, &tm);
#endif

    char offset_buf[7];
    std::strftime(offset_buf, sizeof(offset_buf), "%z", &tm);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.'
        << std::setw(6) << std::setfill('0') << micros;
    if (offset_buf[0] != '\0') {
        out << std::string(offset_buf, 3) << ':' << std::string(offset_buf + 3, 2);
    }
    return out.str();
}

std::string format_uptime_seconds(double seconds) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << seconds;
    return out.str();
}

}  // namespace

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
        report["memory_alloc_mb"] = 0;
        report["memory_heap_mb"] = 0;
        report["memory_total_mb"] = 0;
        report["num_goroutines"] = 0;
        report["timestamp"] = iso_timestamp_now();
        report["uptime_seconds"] = format_uptime_seconds(stats->uptime_seconds);
        report["search_count"] = stats->search_count;
        report["avg_search_time_ms"] = stats->search_count > 0
                                           ? nlohmann::json(stats->avg_search_time_ms)
                                           : nlohmann::json(0);
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
    std::printf("  Goroutines:       0\n");

    std::printf("\nMemory Usage:\n");
    std::printf("  Allocated:        0.0 MB\n");
    std::printf("  Heap:             0.0 MB\n");
    std::printf("  Total system:     0.0 MB\n");

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
