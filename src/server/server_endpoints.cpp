#include <lci/server/server.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <thread>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include <lci/core/reference_tracker.h>
#include <lci/core/text.h>
#include <lci/pagination.h>
#include <lci/file_info.h>
#include <lci/git/analyzer.h>
#include <lci/git/provider.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/language_map.h>
#include <lci/search/search_engine.h>
#include <lci/search/search_options.h>
#include <lci/server/client.h>
#include <lci/server/request_decode.h>
#include <lci/version.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#else
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <fstream>

namespace lci {

namespace {

double get_rss_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
    return 0.0;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
    }
    return 0.0;
#else
    std::ifstream statm("/proc/self/statm");
    if (statm.is_open()) {
        long pages = 0;
        statm >> pages;  // first field is total size, second is RSS
        statm >> pages;
        long page_size = sysconf(_SC_PAGESIZE);
        return static_cast<double>(pages * page_size) / (1024.0 * 1024.0);
    }
    return 0.0;
#endif
}

}  // namespace

// -- Endpoint: /ping ----------------------------------------------------------

void IndexServer::handle_ping(const httplib::Request& /*req*/,
                               httplib::Response& res) {
    auto elapsed = std::chrono::steady_clock::now() - start_time_;
    double uptime = std::chrono::duration<double>(elapsed).count();

    std::string bid = build_id_override_.empty() ? build_id()
                                                 : build_id_override_;

    nlohmann::json j;
    j["uptime_seconds"] = uptime;
    j["version"] = kVersion;
    j["build_id"] = bid;
    json_response(res, j);
}

// -- Endpoint: /status --------------------------------------------------------

void IndexServer::handle_status(const httplib::Request& /*req*/,
                                 httplib::Response& res) {
    bool active = indexing_active_.load(std::memory_order_acquire);
    bool ready = false;
    {
        std::shared_lock lock(mu_);
        ready = search_engine_ != nullptr;
    }

    int fc = 0;
    int sc = 0;
    if (ready) {
        auto stats = indexer_->get_stats();
        fc = stats.total_files;
        sc = stats.total_symbols;
    }

    // Live indexing progress. Reads through MasterIndex::get_progress
    // which atomically forwards to the active pipeline's
    // ProgressTracker (lock-free hot path) when a run is in flight, or
    // returns an idle/zero snapshot otherwise. Polling /status during
    // a long index reports increasing files_scanned without racing the
    // pipeline writer.
    auto progress = indexer_->get_progress();
    auto phase_to_string =
        [](MasterIndex::IndexingPhase phase) -> const char* {
            switch (phase) {
                case MasterIndex::IndexingPhase::Scanning: return "scanning";
                case MasterIndex::IndexingPhase::Indexing: return "indexing";
                case MasterIndex::IndexingPhase::Merging:  return "merging";
                case MasterIndex::IndexingPhase::Idle:     return "idle";
            }
            return "idle";
        };

    nlohmann::json indexing_progress;
    indexing_progress["phase"] = phase_to_string(progress.phase);
    indexing_progress["files_scanned"] = progress.files_scanned;
    indexing_progress["files_total"] = progress.files_total;
    indexing_progress["percent_complete"] = progress.percent_complete;
    indexing_progress["elapsed_ms"] = progress.elapsed_ms;

    nlohmann::json j;
    j["ready"] = ready;
    j["file_count"] = fc;
    j["symbol_count"] = sc;
    j["indexing_active"] = active;
    j["progress"] = ready ? 1.0 : 0.0;
    j["indexing_progress"] = std::move(indexing_progress);
    json_response(res, j);
}

// -- Endpoint: /symbol --------------------------------------------------------

void IndexServer::handle_symbol(const httplib::Request& req,
                                 httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    uint64_t symbol_id = body.value("symbol_id", uint64_t{0});
    if (symbol_id == 0) {
        error_response(res, 400, "symbol_id is required");
        return;
    }

    auto rt_snap = indexer_->ref_tracker().pin();
    auto sym = rt_snap->get_enhanced_symbol(symbol_id);
    if (sym == nullptr) {
        error_response(res, 404, "symbol not found");
        return;
    }

    nlohmann::json j;
    j["symbol"]["name"] = sym->symbol.name;
    j["symbol"]["type"] = std::string(to_string(sym->symbol.type));
    j["symbol"]["file_id"] = sym->symbol.file_id;
    j["symbol"]["line"] = sym->symbol.line;
    j["symbol"]["signature"] = sym->signature;
    j["symbol"]["is_exported"] = sym->is_exported;
    j["symbol"]["doc_comment"] = sym->doc_comment;
    json_response(res, j);
}

// -- Endpoint: /fileinfo ------------------------------------------------------

void IndexServer::handle_fileinfo(const httplib::Request& req,
                                   httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    auto file_id = static_cast<FileID>(body.value("file_id", 0u));
    if (file_id == 0) {
        error_response(res, 400, "file_id is required");
        return;
    }

    auto path = indexer_->get_file_path(file_id);
    if (path.empty()) {
        error_response(res, 404, "file not found");
        return;
    }

    auto rt_snap = indexer_->ref_tracker().pin();
    auto symbols = rt_snap->get_file_enhanced_symbols(file_id);

    nlohmann::json j;
    j["file_info"]["file_id"] = file_id;
    j["file_info"]["path"] = path;
    j["file_info"]["symbol_count"] = static_cast<int>(symbols.size());
    json_response(res, j);
}

// -- Endpoint: /shutdown ------------------------------------------------------

void IndexServer::handle_shutdown(const httplib::Request& req,
                                   httplib::Response& res) {
    bool force = false;
    try {
        auto body = nlohmann::json::parse(req.body);
        force = body.value("force", false);
    } catch (const nlohmann::json::exception&) {
        // Empty/absent body means a plain graceful shutdown.
    }

    nlohmann::json j;
    j["success"] = true;
    j["message"] = "Server shutting down";
    json_response(res, j);

    // Trigger shutdown after the response flushes. Owned (not detached) and
    // joined in shutdown()/dtor, so it can never outlive this server and touch
    // freed members. CAS guards a single spawn across concurrent /shutdown
    // requests (a second request must not overwrite a joinable thread).
    bool expected = false;
    if (shutdown_triggered_.compare_exchange_strong(expected, true)) {
        shutdown_trigger_ = std::thread([this, force] {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            // Clear running_ too: the CLI serve loop exits on
            // !is_running(), and before this /shutdown only flipped
            // shutdown_requested_ (observed by wait(), which the CLI
            // does not call) — a remote /shutdown left the process
            // serving forever. Peer eviction depends on this working.
            running_.store(false, std::memory_order_release);
            // Stop accepting now, whether or not any owner polls
            // is_running() — an embedded server with no watching owner
            // must not keep serving after promising to shut down.
            svr_.stop();
            {
                std::lock_guard lock(shutdown_mu_);
                shutdown_requested_ = true;
            }
            shutdown_cv_.notify_all();
            if (force) {
                // force means "guarantee this process dies": arm a watchdog
                // that exits hard after a grace window. A healthy server
                // tears down and exits normally well within it; only a hung
                // teardown reaches the _Exit. Detaching is safe here — the
                // watchdog captures nothing and touches no members, so it
                // cannot use-after-free (unlike the trigger thread itself,
                // which is owned and joined for exactly that reason).
                std::thread([] {
                    std::this_thread::sleep_for(std::chrono::seconds{5});
                    std::_Exit(0);
                }).detach();
            }
        });
    }
}

// -- Endpoint: /reindex -------------------------------------------------------

void IndexServer::handle_reindex(const httplib::Request& req,
                                  httplib::Response& res) {
    // A server that has decided to stop (self-stop, /shutdown in flight)
    // must refuse: the swap below would cancel-and-join the new run
    // immediately AFTER the lambda already cleared the engine and the
    // index, leaving a permanently bricked server (engine null,
    // indexing_active_ stuck true, every endpoint 503).
    if (!running_.load(std::memory_order_acquire)) {
        error_response(res, 503, "server is shutting down");
        return;
    }

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        body = nlohmann::json::object();
    }

    std::string root_path = body.value("path", "");
    if (root_path.empty()) {
        root_path = config_.project.root;
    }

    // Atomically cancel any prior in-flight indexing run and install
    // the fresh one. Without atomic cancel-and-replace, two concurrent
    // reindex requests would race: both could observe an empty slot
    // and try to install their own thread, terminating the process on
    // the second assignment to a joinable thread. swap_indexing_thread
    // serialises this on `indexing_thread_mu_`.
    //
    // `indexing_active_` is set to true *before* the swap so observers
    // never see a transient false between "cancelled prior run" and
    // "started new run". The lambda only clears it when the run reaches
    // completion (either by publishing a new search engine or by
    // observing shutdown); a cancelled run leaves the flag set so the
    // successor thread covers it.
    indexing_active_.store(true, std::memory_order_release);
    swap_indexing_thread(std::thread([this, root_path] {
        {
            std::unique_lock engine_lock(mu_);
            search_engine_ = nullptr;
            owned_search_engine_.reset();
        }

        indexer_->clear();
        indexer_->index_directory(root_path);

        // Bail out (without clearing indexing_active_) if a successor
        // reindex superseded us — the successor is responsible for
        // clearing the flag once it publishes its own engine. If the
        // server is shutting down, clear the flag so /status is
        // accurate during the brief window before the server exits.
        if (indexer_->stop_requested()) {
            return;
        }
        if (!running_.load(std::memory_order_acquire)) {
            indexing_active_.store(false, std::memory_order_release);
            return;
        }

        auto engine = std::make_unique<SearchEngine>(*indexer_);
        {
            std::unique_lock engine_lock(mu_);
            owned_search_engine_ = std::move(engine);
            search_engine_ = owned_search_engine_.get();
        }
        indexing_active_.store(false, std::memory_order_release);
    }));

    nlohmann::json j;
    j["success"] = true;
    j["message"] = "Re-indexing started for " + root_path;
    json_response(res, j);
}

// -- Endpoint: /stats ---------------------------------------------------------

void IndexServer::handle_stats(const httplib::Request& /*req*/,
                                httplib::Response& res) {
    if (!require_ready(res)) return;

    auto stats = indexer_->get_stats();
    auto elapsed = std::chrono::steady_clock::now() - start_time_;
    double uptime = std::chrono::duration<double>(elapsed).count();

    nlohmann::json j;
    j["file_count"] = stats.total_files;
    j["symbol_count"] = stats.total_symbols;
    j["index_size_bytes"] =
        static_cast<int64_t>(indexer_->index_size_bytes());
    j["build_duration_ms"] = stats.indexing_time_ns / 1'000'000;
    j["memory_rss_mb"] = get_rss_mb();
    j["num_threads"] = static_cast<int>(std::thread::hardware_concurrency());
    j["uptime_seconds"] = uptime;
    json_response(res, j);
}

// -- Endpoint: /tree ----------------------------------------------------------

void IndexServer::handle_tree(const httplib::Request& req,
                               httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    auto function_name = body.value("function_name", "");
    if (function_name.empty()) {
        error_response(res, 400, "function_name is required");
        return;
    }

    int max_depth = body.value("max_depth", 0);

    // Find the symbol by name
    auto rt_snap = indexer_->ref_tracker().pin();
    auto symbols = rt_snap->find_symbols_by_name(function_name);
    if (symbols.empty()) {
        nlohmann::json j;
        j["error"] = "function not found: " + function_name;
        json_response(res, j);
        return;
    }

    const auto& sym = symbols[0];
    auto tree = indexer_->ref_tracker().build_function_tree(
        sym->id, max_depth > 0 ? max_depth : 10);

    // Serialize tree recursively. Mirrors Go's tree node shape, which
    // includes annotations / safety / impact fields (left null/zero in
    // the C++ port until the analyzers that produce them are wired in).
    std::function<int(const FunctionTreeNode&)> count_nodes;
    count_nodes = [&](const FunctionTreeNode& node) -> int {
        int n = 1;
        for (const auto& child : node.children) {
            n += count_nodes(child);
        }
        return n;
    };

    // Normalize file_path to relative-to-project-root so tree paths
    // match what /search and /git-analyze emit.
    auto rel_tree_path = [&](const std::string& p) -> std::string {
        if (p.empty()) return p;
        std::filesystem::path abs_path(p);
        if (!abs_path.is_absolute()) return p;
        std::error_code ec;
        auto rel = std::filesystem::relative(abs_path,
                                             config_.project.root, ec);
        if (ec || rel.empty()) return p;
        return rel.generic_string();
    };

    std::function<nlohmann::json(const FunctionTreeNode&, int)> serialize_node;
    serialize_node = [&](const FunctionTreeNode& node, int depth) -> nlohmann::json {
        nlohmann::json nj;
        nj["name"] = node.name;
        nj["line"] = node.line;
        nj["depth"] = depth;
        // Resolve file_path from the node's file_id so CLI consumers can
        // render `[path:line]` annotations and look up per-symbol metrics
        // via /browse-file. Empty string for unresolved nodes (root with
        // no symbol bound, recursion guards, etc.).
        nj["file_path"] =
            node.file_id != 0
                ? rel_tree_path(indexer_->get_file_path(node.file_id))
                : "";
        nj["node_type"] = 0;
        nj["dependency_count"] = static_cast<int>(node.children.size());
        nj["dependent_count"] = 0;
        nj["edit_risk_score"] = 0;
        nj["impact_radius"] = 0;
        nj["annotations"] = nullptr;
        nj["safety_notes"] = nullptr;
        nj["stability_tags"] = nullptr;

        nlohmann::json children = nlohmann::json::array();
        for (const auto& child : node.children) {
            children.push_back(serialize_node(child, depth + 1));
        }
        nj["children"] = children;
        return nj;
    };

    int total_nodes = count_nodes(tree) - 1;  // exclude root from count

    nlohmann::json options;
    options["agent_mode"] = false;
    options["compact"] = false;
    options["exclude_pattern"] = "";
    options["max_depth"] = max_depth;
    options["show_lines"] = false;

    nlohmann::json tree_j;
    tree_j["root"] = serialize_node(tree, 0);
    tree_j["root_function"] = function_name;
    tree_j["max_depth"] = max_depth;
    tree_j["options"] = options;
    tree_j["total_nodes"] = total_nodes;

    nlohmann::json j;
    j["tree"] = tree_j;
    json_response(res, j);
}

// -- Endpoint: /git-analyze ---------------------------------------------------

void IndexServer::handle_git_analyze(const httplib::Request& req,
                                       httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    auto scope = body.value("scope", "");
    if (scope.empty()) {
        error_response(res, 400, "scope is required");
        return;
    }

    git::Provider provider;
    if (!git::Provider::create(config_.project.root, provider)) {
        // Absent precondition, not a request error: 200 with an explicit
        // not-applicable payload, matching the MCP git surfaces.
        nlohmann::json na;
        na["available"] = false;
        na["reason"] = "not a git repository";
        json_response(res, na);
        return;
    }

    git::AnalysisParams params = git::AnalysisParams::defaults();
    if (scope == "staged") {
        params.scope = git::AnalysisScope::Staged;
    } else if (scope == "wip") {
        params.scope = git::AnalysisScope::WIP;
    } else if (scope == "commit") {
        params.scope = git::AnalysisScope::Commit;
    } else if (scope == "range") {
        params.scope = git::AnalysisScope::Range;
    } else {
        error_response(res, 400, "invalid scope");
        return;
    }

    params.base_ref = body.value("base_ref", "");
    params.target_ref = body.value("target_ref", "");
    params.similarity_threshold = body.value("similarity_threshold", 0.8);
    params.max_findings = body.value("max_findings", 20);
    if (body.contains("focus") && body["focus"].is_array()) {
        params.focus = body["focus"].get<std::vector<std::string>>();
    }

    git::Analyzer analyzer(provider, *indexer_);
    git::AnalysisReport report;
    if (!analyzer.analyze(params, report)) {
        error_response(res, 500, "git analyze failed");
        return;
    }

    nlohmann::json j;
    j["report"] = git::report_to_json(report, config_.project.root);
    json_response(res, j);
}

}  // namespace lci
