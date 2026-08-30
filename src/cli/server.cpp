#include <lci/cli/commands.h>
#include <lci/core/portable.h>
#include <lci/core/subprocess.h>
#include <lci/server/server.h>
#include <lci/version.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstdlib>

namespace lci {
namespace cli {

namespace {

std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int /*sig*/) { g_shutdown_requested.store(true); }

/// Seconds since a registry entry was last touched. Activity stamping is
/// throttled to 60s, so this is a coarse "last used", not a precise idle clock.
long long idle_seconds(const ServerInstance& inst) {
    const auto now = std::filesystem::file_time_type::clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(
               now - inst.last_activity)
        .count();
}

}  // namespace

// -- Instance registry directory ------------------------------------------------

std::string instance_registry_dir() {
    namespace fs = std::filesystem;
    fs::path dir;
#ifndef _WIN32
    // Prefer the kernel-managed per-user runtime dir; fall back to a
    // uid-suffixed private dir under temp. Either way the directory is
    // 0700: registry entries drive shutdown/eviction, so they must not be
    // forgeable by other local users (the old location — the shared system
    // temp dir itself — allowed exactly that).
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR");
        xdg != nullptr && *xdg != '\0') {
        dir = fs::path(xdg) / "lci";
    } else {
        dir = fs::temp_directory_path() /
              ("lci-registry-" + std::to_string(::getuid()));
    }
#else
    // %TEMP% is already per-user on Windows.
    dir = fs::temp_directory_path() / "lci-registry";
#endif
    std::error_code ec;
    fs::create_directories(dir, ec);
#ifndef _WIN32
    ::chmod(dir.c_str(), 0700);
#endif
    return dir.string();
}

// -- ensure_server_running ----------------------------------------------------

std::unique_ptr<Client> ensure_server_running(const Config& cfg,
                                              std::string& error) {
    std::string socket_path = get_socket_path_for_root(cfg.project.root);
    auto client = std::make_unique<Client>(socket_path);

    if (client->is_server_running()) {
        std::string ping_err;
        auto ping = client->ping(ping_err);
        if (ping && !ping->build_id_value.empty() &&
            ping->build_id_value != build_id()) {
            std::fprintf(stderr,
                         "Stale server detected (build %s != %s), "
                         "restarting...\n",
                         ping->build_id_value.c_str(), build_id().c_str());
            std::string shutdown_err;
            client->shutdown(false, shutdown_err);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
#ifndef _WIN32
            ::unlink(socket_path.c_str());
#endif
        } else {
            return client;
        }
    }

    std::fprintf(stderr,
                 "Index server not running, starting in background...\n");

    std::filesystem::path exe;
    try {
        exe = portable::executable_path();
    } catch (const std::runtime_error& e) {
        error = std::string("failed to get executable path: ") + e.what();
        return nullptr;
    }

    std::vector<std::string> argv{exe.string()};
    if (!cfg.project.root.empty() && cfg.project.root != ".") {
        argv.push_back("--root");
        argv.push_back(cfg.project.root);
    }
    argv.push_back("server");

    if (!subprocess::spawn_detached(argv)) {
        error = "failed to spawn background server process";
        return nullptr;
    }

    std::fprintf(stderr, "Waiting for index server to be ready...\n");

    int ready_timeout_sec = cfg.performance.indexing_timeout_sec > 0
                                ? cfg.performance.indexing_timeout_sec
                                : 30;

    std::string wait_err;
    if (!client->wait_for_ready(std::chrono::seconds(ready_timeout_sec),
                                wait_err)) {
        error = "server did not become ready: " + wait_err;
        return nullptr;
    }

    std::fprintf(stderr, "Index server ready\n");
    return client;
}

// -- Server start -------------------------------------------------------------

int run_server(const GlobalFlags& flags, bool daemon, bool foreground) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    // -- daemon / foreground resolution -----------------------------------
    //
    // Go cmd/lci/main.go:801-811 declares both; `--foreground` defaults true.
    // We treat `--foreground` as an explicit override: when set, daemon mode
    // is ignored (with a stderr notice on conflict). When neither is set,
    // we run in foreground — matches Go's default + matches user expectation
    // for `lci server` invoked interactively.
    if (daemon && foreground) {
        std::fprintf(stderr,
                     "Note: --foreground overrides --daemon; running in "
                     "foreground for debug.\n");
        daemon = false;
    }

    if (daemon) {
        // Daemonize: spawn a fully detached copy of ourselves running the
        // server loop in the foreground (--foreground short-circuits any
        // config default that might re-enable daemon mode), then return so
        // the launching process exits immediately.
        std::filesystem::path exe;
        try {
            exe = portable::executable_path();
        } catch (const std::runtime_error& e) {
            std::cerr << "Error: failed to get executable path: " << e.what()
                      << "\n";
            return 1;
        }

        std::vector<std::string> argv{exe.string()};
        if (!cfg.project.root.empty() && cfg.project.root != ".") {
            argv.push_back("--root");
            argv.push_back(cfg.project.root);
        }
        argv.push_back("server");
        argv.push_back("--foreground");

        if (!subprocess::spawn_detached(argv)) {
            std::cerr << "Error: failed to spawn background server process\n";
            return 1;
        }
        std::printf("Index server starting in background\n");
        return 0;
    }

    IndexServer server(cfg);
    std::string socket_path = get_socket_path_for_root(cfg.project.root);
    server.set_socket_path(socket_path);
    // Real (CLI-launched) servers participate in the per-user instance
    // registry so the least-recently-active ones get evicted past
    // server.max_instances. Embedded/test servers stay out unless they
    // opt in with their own directory.
    server.enable_instance_registry(instance_registry_dir());

    if (!server.start()) {
        std::cerr << "Error: failed to start server\n";
        return 1;
    }

    std::printf("Index server started successfully\n");
    std::printf("Socket: %s\n", socket_path.c_str());
    std::printf("Root: %s\n", cfg.project.root.c_str());
    std::printf("\nUse 'lci shutdown' to stop the server\n");

    g_shutdown_requested.store(false);
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Wait for shutdown signal or server-initiated shutdown
    while (!g_shutdown_requested.load() && server.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (g_shutdown_requested.load()) {
        std::printf("\nReceived shutdown signal, shutting down...\n");
    } else {
        std::printf("Server shutdown requested\n");
    }

    if (!server.shutdown()) {
        std::cerr << "Warning: shutdown did not complete cleanly\n";
    }

    std::printf("Server shut down cleanly\n");
    return 0;
}

// -- Shutdown -----------------------------------------------------------------

int run_shutdown(const GlobalFlags& flags, bool force) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::string socket_path = get_socket_path_for_root(cfg.project.root);
    Client client(socket_path);

    if (!client.is_server_running()) {
        // Path-derivation fallback: a live server for this root can sit on
        // a different address than the one we derive (root string resolved
        // differently at its spawn — symlinks, trailing slashes). The
        // instance registry records the address each server actually bound,
        // so match by root there before declaring nothing running.
        std::error_code canon_ec;
        auto wanted = std::filesystem::weakly_canonical(cfg.project.root,
                                                        canon_ec);
        bool found = false;
        for (const auto& inst :
             list_server_instances(instance_registry_dir())) {
            std::error_code ec2;
            if (inst.root.empty()) continue;
            if (std::filesystem::weakly_canonical(inst.root, ec2) != wanted)
                continue;
            Client candidate(inst.address);
            if (candidate.is_server_running()) {
                socket_path = inst.address;
                client = Client(socket_path);
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "Error: no server is running for root: "
                      << cfg.project.root << "\n";
            return 1;
        }
    }

    std::printf("Shutting down server for root: %s\n",
                cfg.project.root.c_str());

    std::string shutdown_err;
    if (!client.shutdown(force, shutdown_err)) {
        std::cerr << "Error: failed to shutdown server: " << shutdown_err
                  << "\n";
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (client.is_server_running()) {
        std::cerr << "Error: server did not shut down\n";
        return 1;
    }

    std::printf("Server shut down successfully\n");
    return 0;
}

// -- Fleet-wide listing and shutdown ------------------------------------------

int run_servers(bool json_output) {
    auto live = list_server_instances(instance_registry_dir());

    if (json_output) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& inst : live) {
            out.push_back({{"pid", inst.pid},
                           {"address", inst.address},
                           {"root", inst.root},
                           {"root_exists", inst.root_exists},
                           {"idle_seconds", idle_seconds(inst)}});
        }
        std::printf("%s\n", out.dump(2).c_str());
        return 0;
    }

    if (live.empty()) {
        std::printf("No index servers running\n");
        return 0;
    }

    std::printf("%-8s %-10s %s\n", "PID", "IDLE", "ROOT");
    for (const auto& inst : live) {
        std::printf("%-8u %-10lld %s%s\n", inst.pid, idle_seconds(inst),
                    inst.root.empty() ? "(unknown)" : inst.root.c_str(),
                    inst.root_exists ? "" : "  [root deleted]");
    }
    std::printf("\n%zu server(s). Use 'lci shutdown --all' to stop them.\n",
                live.size());
    return 0;
}

int run_shutdown_all(bool force) {
    auto live = list_server_instances(instance_registry_dir());
    if (live.empty()) {
        std::printf("No index servers running\n");
        return 0;
    }

    int failed = 0;
    for (const auto& inst : live) {
        Client client(inst.address);
        std::string err;
        if (!client.shutdown(force, err)) {
            std::fprintf(stderr, "Error: pid %u (%s): %s\n", inst.pid,
                         inst.root.c_str(), err.c_str());
            ++failed;
            continue;
        }
        std::printf("Stopped pid %u  %s\n", inst.pid,
                    inst.root.empty() ? "(unknown root)" : inst.root.c_str());
    }

    // Shutdown is asynchronous on the server side (the handler answers, then a
    // trigger thread tears the listener down), so confirm rather than assume.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int survivors = 0;
    for (const auto& inst : live) {
        if (Client(inst.address).is_server_running()) {
            std::fprintf(stderr, "Error: pid %u did not shut down\n", inst.pid);
            ++survivors;
        }
    }

    if (failed > 0 || survivors > 0) {
        return 1;
    }
    std::printf("\n%zu server(s) shut down\n", live.size());
    return 0;
}

}  // namespace cli
}  // namespace lci
