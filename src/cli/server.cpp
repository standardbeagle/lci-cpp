#include <lci/cli/commands.h>
#include <lci/core/portable.h>
#include <lci/core/subprocess.h>
#include <lci/server/server.h>
#include <lci/version.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace lci {
namespace cli {

namespace {

std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int /*sig*/) { g_shutdown_requested.store(true); }

}  // namespace

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

    if (!server.shutdown(std::chrono::milliseconds(10000))) {
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
        std::cerr << "Error: no server is running for root: "
                  << cfg.project.root << "\n";
        return 1;
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

}  // namespace cli
}  // namespace lci
