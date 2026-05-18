#include <lci/cli/commands.h>

#include <chrono>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#if !defined(_WIN32)
#  include <sys/resource.h>
#  include <unistd.h>
#endif

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

// Reads VmRSS / Threads from /proc/self/status. Returns {rss_kb,
// threads}; either may be 0 if unavailable. Linux + WSL; macOS/Windows
// covered separately below.
struct ProcRuntime {
    long rss_kb{};
    int threads{};
};

ProcRuntime read_proc_runtime() {
    ProcRuntime r;
#if defined(__linux__)
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream ss(line.substr(6));
            ss >> r.rss_kb;
        } else if (line.rfind("Threads:", 0) == 0) {
            std::istringstream ss(line.substr(8));
            ss >> r.threads;
        }
    }
#elif !defined(_WIN32)
    // BSD / macOS fallback: getrusage gives ru_maxrss (kilobytes on
    // Linux, bytes on macOS — but we only build this branch off-Linux).
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
#  if defined(__APPLE__)
        r.rss_kb = ru.ru_maxrss / 1024;
#  else
        r.rss_kb = ru.ru_maxrss;
#  endif
    }
    // Thread count not portable here; leave 0.
#endif
    return r;
}

double rss_mb(const ProcRuntime& r) {
    return static_cast<double>(r.rss_kb) / 1024.0;
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

    auto runtime = read_proc_runtime();
    double rss = rss_mb(runtime);

    if (json_output) {
        nlohmann::json report;
        report["ready"] = status->ready;
        report["file_count"] = stats->file_count;
        report["symbol_count"] = stats->symbol_count;
        report["index_size_bytes"] = stats->index_size_bytes;
        report["build_duration_ms"] = stats->build_duration_ms;
        // C++-native runtime metrics. Go reports goroutines + heap; C++
        // reports threads + RSS. Different runtimes — fields named to
        // reflect what's actually measured rather than faking Go's shape
        // with zeros (the prior behavior).
        report["num_threads"] = runtime.threads;
        report["memory_rss_mb"] = rss;
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
    std::printf("  Threads:          %d\n", runtime.threads);

    std::printf("\nMemory Usage:\n");
    std::printf("  RSS:              %.1f MB\n", rss);

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
