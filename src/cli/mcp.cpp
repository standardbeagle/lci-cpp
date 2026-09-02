#include <lci/cli/commands.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_analysis.h>
#include <lci/mcp/runtime.h>
#include <lci/mcp/server.h>
#include <lci/search/search_engine.h>
#include <lci/server/client.h>
#include <lci/server/server.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace lci {
namespace cli {

namespace {

/// Bridges this process's stdio MCP client to a running index server's
/// POST /mcp endpoint: no local index, no re-index — the server's warmed
/// index answers every call. Returns the process exit code, or -1 when
/// the server does not host MCP (stale binary) and the caller should run
/// the legacy in-process path.
int run_mcp_bridge(Client& client) {
    // Probe hosting support with a ping frame before committing.
    std::string probe_resp, probe_err;
    int status = client.mcp_dispatch(
        R"({"jsonrpc":"2.0","id":"lci-bridge-probe","method":"ping"})",
        probe_resp, probe_err);
    if (status != 200) return -1;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::string out, err;
        int st = client.mcp_dispatch(line, out, err);
        if (st == 204) continue;  // notification: no response frame
        if (st == 200) {
            std::cout << out << '\n';
            std::cout.flush();
            continue;
        }
        // Mid-session bridge failure: answer THIS frame with a JSON-RPC
        // error rather than dying silently; notifications and unparseable
        // frames have nothing to answer.
        nlohmann::json id = nullptr;
        try {
            auto req = nlohmann::json::parse(line);
            if (req.contains("id")) id = req["id"];
        } catch (const nlohmann::json::parse_error&) {
        }
        if (id.is_null()) continue;
        nlohmann::json envelope = {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error",
             {{"code", -32603},
              {"message", "bridge to index server failed: " +
                              (st < 0 ? err
                                      : "HTTP " + std::to_string(st))}}}};
        std::cout << envelope.dump() << '\n';
        std::cout.flush();
    }
    return 0;
}

}  // namespace

// FIX-D.1 sweep (Dart FZJ6Iip4we3U): all 8 parity-compat stubs removed —
// find_files, debug_info, list_symbols, inspect_symbol, browse_file,
// git_analysis, side_effects, code_insight. Those stubs once shadowed real
// handlers under the old reverse-iteration last-write-wins dispatch, inflating
// tools/list from Go's 14 to C++'s 22. Real handlers in
// handlers_{core,explore,index,analysis,context}.cpp now own dispatch; the
// final stub registrar (McpServer::register_tools/stub_handler) and the
// reverse-iteration shadow mechanism have since been deleted — dispatch is now
// plain forward iteration over tools each registered exactly once.
// Prior individual removals (iter-5/6/9/14): search,
// get_context, index_stats. The entire register_parity_compat_tools() helper
// and its private stub-only helpers (collect_symbols, basic_symbol_json,
// iso_timestamp_now, etc.) were deleted alongside. See MODULE_MAP.md
// "Decision: tools/list emit-order parity".

int run_mcp(const GlobalFlags& flags) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    // Bridge-first: share the persistent per-root index server (spawning
    // it if needed) so N stdio clients pay for ONE index build instead of
    // N. Skipped when the invocation carries process-local overrides the
    // shared server would not have — a custom config file, include/exclude
    // globs, or LCI_ERROR_REPORT — those keep the in-process path so the
    // overrides actually apply.
    bool has_local_overrides =
        flags.config_path != ".lci.kdl" || !flags.include.empty() ||
        !flags.exclude.empty() ||
        std::getenv("LCI_ERROR_REPORT") != nullptr;
    if (!has_local_overrides) {
        std::string ensure_err;
        if (auto client = ensure_server_running(cfg, ensure_err)) {
            int code = run_mcp_bridge(*client);
            if (code >= 0) return code;
            std::cerr << "Warning: index server does not host MCP (older "
                         "binary?); falling back to an in-process index\n";
        } else {
            std::cerr << "Warning: could not reach or start an index "
                         "server (" << ensure_err
                      << "); falling back to an in-process index\n";
        }
    }

    MasterIndex runtime_index(cfg);
    SearchEngine search_engine(runtime_index, cfg.synonyms);
    mcp::McpRuntime runtime(runtime_index);
    // AST-fact side effects are recorded during indexing (must be wired
    // before index_directory runs in the warmup thread below).
    runtime_index.set_side_effect_sink(&runtime.side_effects);

    // Shared IndexServer so CLI commands can also connect. It SHARES
    // runtime_index instead of owning a second MasterIndex: the owning
    // constructor used to index the same root a second time, concurrently
    // with the warmup below — double CPU/IO and double resident memory for
    // the whole MCP session. With no engine yet it answers 503 "still
    // indexing" (and reports live progress through the shared index) until
    // the warmup publishes the engine.
    //
    // Its lifetime is the MCP session: idle exit is disabled because the MCP
    // client owns this process — an idle-reaped listener would only make the
    // next CLI command spawn a duplicate standalone server for the same root.
    Config server_cfg = cfg;
    server_cfg.server.idle_timeout_sec = 0;
    IndexServer index_server(server_cfg, runtime_index, nullptr);
    index_server.set_socket_path(get_socket_path_for_root(cfg.project.root));
    // Register with the per-user fleet so `lci servers` / `lci shutdown
    // --all` see MCP-hosted servers too, not only CLI-launched ones.
    index_server.enable_instance_registry(instance_registry_dir());

    // Start MCP server with the live in-process index instead of the
    // stub-only registry so parity and stdio users hit the real handlers.
    // Constructed before index_server starts so the /mcp dispatcher below
    // is installed before any request can arrive.
    mcp::WarmupLatch warmup;
    mcp::McpServer mcp_server(cfg, runtime_index, &search_engine);
    mcp::register_all_handlers(mcp_server, &runtime_index, &search_engine,
                               &runtime);

    // Serialises every handler behind the warmup: no handler observes the
    // index while the warmup thread is still writing it, and after the wait
    // nothing mutates it again. The wait is UNBOUNDED and safe to be: the
    // gate runs on McpServer's tool worker thread (or an /mcp bridge
    // request's thread), so the transport keeps answering liveness pings
    // while a tool call waits out the index build. (The previous
    // 20s-timeout gate made a cold one-shot client — pipe requests, close
    // stdin — unable to ever succeed on a large corpus: every retry was a
    // fresh process paying for a full rebuild it then discarded.)
    mcp_server.set_readiness_gate([&warmup](std::string& error) {
        return warmup.wait(error);
    });

    // Host mode also answers POST /mcp, so CONCURRENT `lci mcp` one-shots
    // for the same root bridge to this process instead of each building
    // their own index.
    index_server.set_mcp_dispatcher([&mcp_server](const std::string& line) {
        return mcp_server.dispatch_wire(line);
    });

    // A self-stop (RSS self-cap, project root deleted; idle exit is
    // disabled above) must end THIS process, not just its listener: the
    // stdio loop below blocks in getline and never observes the server's
    // shutdown flag, so before this callback a self-stop left the process
    // alive with the full index resident — exactly the RSS harm the cap
    // exists to prevent. Exit hard, like the standalone server leaving its
    // serve loop: the MCP client sees EOF and respawns on demand.
    index_server.set_self_stop_callback([](const char* reason) {
        std::fprintf(stderr,
                     "lci mcp: shared index server self-stopped (%s); "
                     "exiting\n",
                     reason);
        std::_Exit(0);
    });

    bool shared_server_started = index_server.start();
    if (!shared_server_started) {
        std::cerr << "Warning: failed to start shared index server; "
                     "CLI commands won't be able to connect\n";
    }

    // Index and analysis run OFF the transport thread. Every one of these
    // phases used to run before mcp_server.run() was even called, so stdin was
    // not read until the whole corpus was indexed — on a large repo that
    // outlives an MCP client's connect deadline, and the client gives up on a
    // server that is working fine (observed as "lazy-connect failed for MCP
    // lci: context deadline exceeded", after which slop-mcp parks lci in an
    // error state for 30s). The handshake is cheap and must never wait on the
    // corpus; only tools/call does, via the readiness gate below.
    std::thread warmup_thread([&] {
        if (!runtime_index.index_directory(cfg.project.root)) {
            // Not fatal: handlers over an empty index answer honestly, and the
            // pre-existing behaviour was to warn and serve. Keep that, so a
            // partially-readable tree still gets a usable server.
            std::cerr << "Warning: failed to index project root for MCP "
                         "runtime\n";
        }

        // Annotation extraction, side-effect AST + heuristic passes,
        // transitive propagation, label seeding (see McpRuntime::warmup —
        // shared with the persistent server's MCP hosting).
        runtime.warmup(runtime_index);

        // Flip the shared HTTP server ready now that the shared index is
        // fully built: CLI clients waiting in wait_for_ready unblock here.
        index_server.set_search_engine(&search_engine);

        // An unindexable root still yields a serving (empty) index — the
        // behaviour before warmup moved off the transport thread, kept so a
        // partially-readable tree is not turned into a dead server.
        warmup.finish({});
    });

    int exit_code = mcp_server.run();

    if (shared_server_started) {
        index_server.shutdown();
    }

    // The warmup writes into objects that live on this frame (including
    // index_server via set_search_engine); it must finish before they are
    // destroyed, even when the transport exits early (EOF on stdin during a
    // long first index is the normal way a client gives up).
    warmup_thread.join();

    // BETA error-report capture (insight.error_report = "capture"):
    // generate-don't-publish for batch drivers like err-lookup. Runs after
    // the transport exits and the warmup joined, so the index is complete
    // and no request ever paid for it. No-op in "off" and "on".
    if (std::string p =
            mcp::write_error_report_capture(runtime_index,
                                            &runtime.side_effects);
        !p.empty()) {
        std::cerr << "error report captured: " << p << "\n";
    }

    return exit_code;
}

}  // namespace cli
}  // namespace lci
