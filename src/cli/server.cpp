#include <lci/cli/commands.h>
#include <lci/server/server.h>
#include <lci/version.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

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

#ifndef _WIN32
    // Get path to current executable via /proc/self/exe
    std::error_code ec;
    auto exe_path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        error = "failed to get executable path: " + ec.message();
        return nullptr;
    }

    std::string exe = exe_path.string();
    std::string root_arg;
    if (!cfg.project.root.empty() && cfg.project.root != ".") {
        root_arg = cfg.project.root;
    }

    pid_t pid = fork();
    if (pid < 0) {
        error = "failed to fork server process";
        return nullptr;
    }
    if (pid == 0) {
        // Child: detach and exec server
        setsid();

        // Redirect stdout/stderr to /dev/null for daemon
        auto* f1 = freopen("/dev/null", "w", stdout);
        auto* f2 = freopen("/dev/null", "w", stderr);
        auto* f3 = freopen("/dev/null", "r", stdin);
        (void)f1; (void)f2; (void)f3;

        if (!root_arg.empty()) {
            execl(exe.c_str(), exe.c_str(), "--root", root_arg.c_str(),
                  "server", nullptr);
        } else {
            execl(exe.c_str(), exe.c_str(), "server", nullptr);
        }
        _exit(1);
    }
#else
    error = "background server start not implemented on Windows";
    return nullptr;
#endif

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

int run_server(const GlobalFlags& flags) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
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
