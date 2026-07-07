#include <lci/cli/commands.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <lci/core/portable.h>

namespace lci {
namespace cli {

namespace fs = std::filesystem;

// -- Config loading -----------------------------------------------------------

std::string load_config_with_overrides(const GlobalFlags& flags, Config& out) {
    std::string config_path = flags.config_path;

    if (!flags.root.empty() && config_path == ".lci.kdl") {
        config_path = (fs::path(flags.root) / ".lci.kdl").string();
    }

    std::string root_dir = flags.root.empty() ? fs::current_path().string()
                                              : flags.root;
    auto result = load_config(root_dir);
    if (!result.ok()) {
        return "failed to load config from " + config_path + ": " +
               result.error;
    }
    out = std::move(result.config);

    if (!flags.include.empty()) {
        out.include = flags.include;
    }
    if (!flags.exclude.empty()) {
        out.exclude.insert(out.exclude.end(), flags.exclude.begin(),
                           flags.exclude.end());
    }
    if (!flags.root.empty()) {
        std::error_code ec;
        auto abs_root = fs::absolute(fs::path(flags.root), ec);
        if (ec) {
            return "failed to resolve root path '" + flags.root +
                   "': " + ec.message();
        }
        out.project.root = abs_root.string();
    }
    return {};
}

// -- MCP auto-detection -------------------------------------------------------

bool is_mcp_mode() {
    if (const char* env = std::getenv("LCI_MCP_MODE")) {
        std::string val(env);
        if (val == "1" || val == "true") return true;
    }

    // stdin is not an interactive terminal => almost certainly an MCP client
    // piping JSON-RPC on stdin.
#if defined(_WIN32)
    DWORD type = GetFileType(GetStdHandle(STD_INPUT_HANDLE));
    if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK) return true;
#else
    struct stat st {};
    if (fstat(STDIN_FILENO, &st) == 0 && !S_ISCHR(st.st_mode)) {
        return true;
    }
#endif

    // argv[0] basename hints (an "lci-mcp" / "lci-server" launcher name).
    {
        std::string arg0;
#if defined(__GLIBC__)
        arg0 = fs::path(program_invocation_name).filename().string();
#elif defined(__APPLE__)
        arg0 = getprogname();
#elif defined(_WIN32)
        try {
            arg0 = portable::executable_path().filename().string();
        } catch (const std::runtime_error&) {
            // No exe name available; skip this heuristic.
        }
#endif
        for (auto& c : arg0) c = static_cast<char>(std::tolower(c));
        if (arg0.find("mcp") != std::string::npos ||
            arg0.find("server") != std::string::npos) {
            return true;
        }
    }

    // Linux-only: inspect the parent process name via procfs for known MCP
    // clients. No portable equivalent on macOS/Windows; the stdin + argv0
    // heuristics above cover those platforms.
#if defined(__linux__)
    {
        pid_t ppid = getppid();
        if (ppid > 1) {
            std::string comm_path =
                "/proc/" + std::to_string(ppid) + "/comm";
            std::ifstream f(comm_path);
            if (f) {
                std::string parent_name;
                std::getline(f, parent_name);
                for (auto& c : parent_name)
                    c = static_cast<char>(std::tolower(c));
                static constexpr std::string_view kMcpClients[] = {
                    "mcp-tui", "mcp-client", "claude", "cursor", "vscode"};
                for (auto client : kMcpClients) {
                    if (parent_name.find(client) != std::string::npos)
                        return true;
                }
            }
        }
    }
#endif

    return false;
}

// -- Formatting helpers -------------------------------------------------------

std::string format_bytes(int64_t bytes) {
    constexpr int64_t kKB = 1024;
    constexpr int64_t kMB = 1024 * kKB;
    constexpr int64_t kGB = 1024 * kMB;

    char buf[64];
    if (bytes >= kGB) {
        std::snprintf(buf, sizeof(buf), "%.2f GB",
                      static_cast<double>(bytes) / static_cast<double>(kGB));
    } else if (bytes >= kMB) {
        std::snprintf(buf, sizeof(buf), "%.2f MB",
                      static_cast<double>(bytes) / static_cast<double>(kMB));
    } else if (bytes >= kKB) {
        std::snprintf(buf, sizeof(buf), "%.2f KB",
                      static_cast<double>(bytes) / static_cast<double>(kKB));
    } else {
        std::snprintf(buf, sizeof(buf), "%lld bytes",
                      static_cast<long long>(bytes));
    }
    return buf;
}

std::string format_milliseconds(int64_t ms) {
    char buf[64];
    if (ms < 1000) {
        std::snprintf(buf, sizeof(buf), "%lld ms",
                      static_cast<long long>(ms));
    } else {
        double seconds = static_cast<double>(ms) / 1000.0;
        if (seconds < 60.0) {
            std::snprintf(buf, sizeof(buf), "%.1f seconds", seconds);
        } else {
            double minutes = seconds / 60.0;
            if (minutes < 60.0) {
                std::snprintf(buf, sizeof(buf), "%.1f minutes", minutes);
            } else {
                double hours = minutes / 60.0;
                std::snprintf(buf, sizeof(buf), "%.1f hours", hours);
            }
        }
    }
    return buf;
}

std::string format_seconds(double seconds) {
    char buf[64];
    if (seconds < 60.0) {
        std::snprintf(buf, sizeof(buf), "%.0f seconds", seconds);
    } else {
        double minutes = seconds / 60.0;
        if (minutes < 60.0) {
            std::snprintf(buf, sizeof(buf), "%.1f minutes", minutes);
        } else {
            double hours = minutes / 60.0;
            if (hours < 24.0) {
                std::snprintf(buf, sizeof(buf), "%.1f hours", hours);
            } else {
                double days = hours / 24.0;
                std::snprintf(buf, sizeof(buf), "%.1f days", days);
            }
        }
    }
    return buf;
}

}  // namespace cli
}  // namespace lci
