#include <lci/cli/commands.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_analysis.h>
#include <lci/mcp/runtime.h>
#include <lci/mcp/server.h>
#include <lci/search/search_engine.h>
#include <lci/server/server.h>

#include <iostream>
#include <string>
#include <thread>

namespace lci {
namespace cli {

namespace {}  // namespace

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

    MasterIndex runtime_index(cfg);
    SearchEngine search_engine(runtime_index, cfg.synonyms);
    mcp::McpRuntime runtime(runtime_index);

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
