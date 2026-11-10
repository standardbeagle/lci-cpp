#include <lci/cli/commands.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_analysis.h>
#include <lci/mcp/runtime.h>
#include <lci/mcp/server.h>
#include <lci/mcp/tool_allowlist.h>
#include <lci/search/search_engine.h>
#include <lci/server/client.h>
#include <lci/server/server.h>

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace lci {
namespace cli {

namespace {

/// Reads the names the index server will serve, for checking --tools against
/// the server that answers rather than this binary's own catalogue (a server
/// left running by an older binary can serve a different set). Returns
/// false with `error` filled when the list cannot be read.
bool read_bridged_tool_names(Client& client, std::vector<std::string>& names,
                             std::string& error) {
    std::string resp, err;
    int status = client.mcp_dispatch(
        R"({"jsonrpc":"2.0","id":"lci-bridge-tools","method":"tools/list"})",
        resp, err);
    if (status != 200) {
        error = status < 0 ? err : "HTTP " + std::to_string(status);
        return false;
    }
    auto frame = nlohmann::json::parse(resp, nullptr,
                                       /*allow_exceptions=*/false);
    if (!frame.is_object() || !frame.contains("result") ||
        !frame["result"].is_object() || !frame["result"].contains("tools") ||
        !frame["result"]["tools"].is_array()) {
        error = "tools/list answered without a tools array";
        return false;
    }
    for (const auto& tool : frame["result"]["tools"]) {
        if (tool.contains("name") && tool["name"].is_string()) {
            names.push_back(tool["name"].get<std::string>());
        }
    }
    return true;
}

/// Bridges this process's stdio MCP client to a running index server's
/// POST /mcp endpoint: no local index, no re-index — the server's warmed
/// index answers every call. Returns the process exit code, or -1 when
/// the server does not host MCP (stale binary) and the caller should run
/// the legacy in-process path.
///
/// A non-null `allowlist` narrows the session on THIS side of the bridge:
/// the server is shared by every bridged session, so it cannot hold one
/// session's allowlist, and filtering here also holds against an older
/// server that knows nothing of --tools.
int run_mcp_bridge(Client& client, const mcp::ToolAllowlist* allowlist) {
    // Probe hosting support with a ping frame before committing.
    std::string probe_resp, probe_err;
    int status = client.mcp_dispatch(
        R"({"jsonrpc":"2.0","id":"lci-bridge-probe","method":"ping"})",
        probe_resp, probe_err);
    if (status != 200) return -1;

    if (allowlist) {
        std::vector<std::string> served;
        std::string error;
        if (!read_bridged_tool_names(client, served, error)) {
            std::cerr << "Error: could not read the index server's tools to "
                         "check --tools: "
                      << error << "\n";
            return 1;
        }
        if (std::string unknown = allowlist->unknown_tools_error(served);
            !unknown.empty()) {
            std::cerr << "Error: " << unknown << "\n";
            return 1;
        }
    }

    // Responses are written whole, one line each, from two threads.
    std::mutex out_mu;
    auto write_frame = [&out_mu](const std::string& frame) {
        std::lock_guard<std::mutex> lock(out_mu);
        std::cout << frame << '\n';
        std::cout.flush();
    };

    // Forwards one frame and writes its answer. `is_tools_list` and
    // `called_tool` select the allowlist's narrowing of the reply.
    auto forward = [&](const std::string& line, bool is_tools_list,
                       const std::string& called_tool) {
        std::string out, err;
        int st = client.mcp_dispatch(line, out, err);
        if (st == 204) return;  // notification: no response frame
        if (st == 200) {
            if (allowlist && is_tools_list) {
                out = allowlist->filter_tools_list_frame(out);
            } else if (allowlist && !called_tool.empty()) {
                out = allowlist->narrow_call_response_frame(called_tool, out);
            }
            write_frame(out);
            return;
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
        if (id.is_null()) return;
        nlohmann::json envelope = {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error",
             {{"code", -32603},
              {"message", "bridge to index server failed: " +
                              (st < 0 ? err
                                      : "HTTP " + std::to_string(st))}}}};
        write_frame(envelope.dump());
    };

    // Two lanes, as in McpServer's own stdio loop: tools/call frames are
    // forwarded in arrival order from one worker thread, every other frame
    // (initialize, ping, tools/list, notifications) inline. A tool call can
    // wait server-side for the whole index build; on a single lane it held
    // every later frame, liveness pings included, behind it.
    struct PendingCall {
        std::string line;
        std::string tool;
    };
    std::mutex calls_mu;
    std::condition_variable calls_cv;
    std::deque<PendingCall> calls;
    bool stdin_closed = false;
    std::thread call_lane([&] {
        for (;;) {
            PendingCall call;
            {
                std::unique_lock<std::mutex> lock(calls_mu);
                calls_cv.wait(lock,
                              [&] { return stdin_closed || !calls.empty(); });
                if (calls.empty()) return;
                call = std::move(calls.front());
                calls.pop_front();
            }
            forward(call.line, false, call.tool);
        }
    });

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        auto request = nlohmann::json::parse(line, nullptr,
                                             /*allow_exceptions=*/false);
        if (allowlist) {
            if (std::string refused = allowlist->refused_call_frame(request);
                !refused.empty()) {
                write_frame(refused);
                continue;
            }
        }
        const bool is_object = request.is_object() && request.contains("method");
        if (is_object && request["method"] == "tools/call") {
            std::string called_tool;
            if (request.contains("params") && request["params"].is_object() &&
                request["params"].contains("name") &&
                request["params"]["name"].is_string()) {
                called_tool = request["params"]["name"].get<std::string>();
            }
            bool queued = false;
            {
                std::lock_guard<std::mutex> lock(calls_mu);
                if (calls.size() < mcp::kMaxQueuedToolCalls) {
                    calls.push_back({std::move(line), std::move(called_tool)});
                    queued = true;
                }
            }
            if (queued) {
                calls_cv.notify_one();
            } else if (request.contains("id")) {
                write_frame(nlohmann::json{{"jsonrpc", "2.0"},
                                           {"id", request["id"]},
                                           {"error",
                                            mcp::tool_queue_full_error()}}
                                .dump());
            }
            continue;
        }
        forward(line, is_object && request["method"] == "tools/list", {});
    }
    {
        std::lock_guard<std::mutex> lock(calls_mu);
        stdin_closed = true;
    }
    calls_cv.notify_one();
    call_lane.join();
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

int run_mcp(const GlobalFlags& flags, const McpOptions& options) {
    // --tools is checked for shape before anything starts; its names are
    // checked against whichever server will actually answer (below).
    std::optional<mcp::ToolAllowlist> allowlist;
    if (options.tools) {
        std::string error;
        allowlist = mcp::ToolAllowlist::parse(*options.tools, error);
        if (!allowlist) {
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
    }
    const mcp::ToolAllowlist* allowlist_ptr =
        allowlist ? &*allowlist : nullptr;

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
        // Listening is enough: the server's readiness gate holds tool calls
        // until its index is built, and the handshake must not wait on it.
        if (auto client = ensure_server_running(cfg, GlobalFlags{}, ensure_err,
                                                ServerWait::Listening)) {
            int code = run_mcp_bridge(*client, allowlist_ptr);
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
    if (allowlist) {
        std::vector<std::string> registered;
        registered.reserve(mcp_server.tool_count());
        for (size_t i = 0; i < mcp_server.tool_count(); ++i) {
            registered.push_back(mcp_server.tool_at(i).name);
        }
        // Refused before the shared server starts or the index is built.
        if (std::string unknown = allowlist->unknown_tools_error(registered);
            !unknown.empty()) {
            std::cerr << "Error: " << unknown << "\n";
            return 1;
        }
        mcp_server.set_stdio_tool_allowlist(*allowlist);
    }

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
