#include <lci/cli/commands.h>
#include <lci/core/portable.h>
#include <lci/core/subprocess.h>
#include <lci/mcp/runtime.h>
#include <lci/mcp/server.h>
#include <lci/search/search_engine.h>
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

        // Wrong-root guard: the socket name is a 31-hash of the project
        // root (a 1000-slot port window on Windows), so two roots can
        // collide on one address. A collision used to be silent — every
        // command searched the OTHER project. /ping now reports the root
        // the server actually serves; on mismatch, find OUR server through
        // the instance registry (each server records its real address
        // there), and fail loudly if it has none. Old binaries omit the
        // field (empty root) and keep the previous trust-the-address
        // behaviour.
        if (ping && !ping->root.empty()) {
            std::error_code ec_want, ec_got;
            auto wanted = std::filesystem::weakly_canonical(cfg.project.root,
                                                            ec_want);
            auto got =
                std::filesystem::weakly_canonical(ping->root, ec_got);
            if (!ec_want && !ec_got && wanted != got) {
                for (const auto& inst :
                     list_server_instances(instance_registry_dir())) {
                    if (inst.root.empty()) continue;
                    std::error_code ec_inst;
                    if (std::filesystem::weakly_canonical(inst.root,
                                                          ec_inst) != wanted)
                        continue;
                    auto candidate = std::make_unique<Client>(inst.address);
                    if (candidate->is_server_running()) {
                        return candidate;
                    }
                }
                error = "socket address collision: the server at " +
                        socket_path + " serves root '" + ping->root +
                        "', not '" + cfg.project.root +
                        "', and no registered server for this root is "
                        "running";
                return nullptr;
            }
        }

        if (ping && !ping->build_id_value.empty() &&
            ping->build_id_value != build_id()) {
            std::fprintf(stderr,
                         "Stale server detected (build %s != %s), "
                         "restarting...\n",
                         ping->build_id_value.c_str(), build_id().c_str());
            std::string shutdown_err;
            client->shutdown(false, shutdown_err);
            // Wait for the old server to actually EXIT before touching its
            // socket: unlinking while it still serves orphans it — alive,
            // unreachable, holding the start lock — and the root is dead to
            // new servers until its idle reaper fires (observed as a
            // ~30-minute outage window).
            bool gone = false;
            for (int i = 0; i < 20; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                if (!client->is_server_running()) {
                    gone = true;
                    break;
                }
            }
            if (!gone) {
                std::fprintf(stderr,
                             "Stale server did not exit within 5s; "
                             "continuing with the running (older-build) "
                             "server instead of orphaning it\n");
                return client;
            }
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

    // Two distinct waits, two distinct failures. Conflating them made a
    // healthy server on a large corpus report "did not become ready" when
    // only the INDEX was still building past the timeout.
    //
    // Phase 1 — spawn-ready: the spawned process binds its socket and
    // answers /ping. Corpus-size independent; a healthy spawn is up within
    // seconds, so a small fixed bound is right.
    constexpr auto kSpawnReadyTimeout = std::chrono::seconds(30);
    constexpr auto kSpawnPollInterval = std::chrono::milliseconds(250);
    const auto spawn_deadline =
        std::chrono::steady_clock::now() + kSpawnReadyTimeout;
    bool listening = false;
    while (std::chrono::steady_clock::now() < spawn_deadline) {
        if (client->is_server_running()) {
            listening = true;
            break;
        }
        std::this_thread::sleep_for(kSpawnPollInterval);
    }
    if (!listening) {
        error = "server process did not start listening within 30s";
        return nullptr;
    }

    // Phase 2 — index-ready: wait_for_ready's timeout bounds STALL, not
    // wall clock, so a big corpus that keeps making progress is fine; only
    // a genuinely stuck index trips it.
    int index_timeout_sec = cfg.performance.indexing_timeout_sec > 0
                                ? cfg.performance.indexing_timeout_sec
                                : 30;

    std::string wait_err;
    if (!client->wait_for_ready(std::chrono::seconds(index_timeout_sec),
                                wait_err)) {
        error = "server started, but its index is not ready: " + wait_err;
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

    // MCP hosting: this server also answers POST /mcp, so `lci mcp`
    // processes bridge their stdio clients here and share ONE warmed
    // index per root instead of re-indexing per stdio process (the
    // err-lookup batch pattern: every one-shot paid a full index build).
    // Tool calls block on the latch until the index and the analysis
    // runtime are warm.
    SearchEngine mcp_engine(server.index(), cfg.synonyms);
    mcp::McpRuntime mcp_runtime(server.index());
    // AST-fact side effects are recorded during the server's own index
    // build (wired before start(), which kicks that build off).
    server.index().set_side_effect_sink(&mcp_runtime.side_effects);
    mcp::McpServer mcp_registry(cfg, server.index(), &mcp_engine);
    mcp::register_all_handlers(mcp_registry, &server.index(), &mcp_engine,
                               &mcp_runtime);
    mcp::WarmupLatch mcp_warm_latch;
    mcp_registry.set_readiness_gate([&mcp_warm_latch](std::string& error) {
        return mcp_warm_latch.wait(error);
    });
    server.set_mcp_dispatcher([&mcp_registry](const std::string& line) {
        return mcp_registry.dispatch_wire(line);
    });

    if (!server.start()) {
        std::cerr << "Error: failed to start server\n";
        return 1;
    }

    // Warm the MCP runtime once the server's own index build completes.
    std::thread mcp_warm_thread([&] {
        while (server.is_running() && !server.is_ready()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (server.is_ready()) {
            mcp_runtime.warmup(server.index());
            mcp_warm_latch.finish({});
        } else {
            mcp_warm_latch.finish(
                "server stopped before its index build completed");
        }
    });

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

    // The warmup thread reads server state and the shared index; it must
    // finish before either is destroyed (it exits promptly once
    // is_running() drops).
    mcp_warm_thread.join();

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
