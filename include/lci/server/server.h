#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <lci/config.h>
#include <lci/types.h>

namespace lci {

class MasterIndex;
class SearchEngine;

// -- Socket path helpers ------------------------------------------------------

/// Returns the default Unix socket path for the LCI server.
std::string get_socket_path();

/// Returns a project-specific socket path based on the root directory.
/// Allows multiple servers for different projects simultaneously.
std::string get_socket_path_for_root(const std::string& root);

// -- Build ID -----------------------------------------------------------------

/// Returns the build ID (compile-time timestamp hash).
/// Used for stale-server detection.
std::string build_id();

// -- JSON request/response types ----------------------------------------------

struct PingResponse {
    double uptime_seconds{};
    std::string version;
    std::string build_id_value;
};

struct IndexStatus {
    bool ready{};
    int file_count{};
    int symbol_count{};
    bool indexing_active{};
    double progress{};
    std::string error;
};

struct SearchRequest {
    std::string pattern;
    int max_results{};
    bool case_insensitive{};
    bool declaration_only{};
};

struct GetSymbolRequest {
    uint64_t symbol_id{};
};

struct GetFileInfoRequest {
    uint32_t file_id{};
};

struct ShutdownRequest {
    bool force{};
};

struct ShutdownResponse {
    bool success{};
    std::string message;
};

struct ReindexRequest {
    std::string path;
};

struct ReindexResponse {
    bool success{};
    std::string message;
};

struct DefinitionRequest {
    std::string pattern;
    int max_results{};
};

struct DefinitionLocation {
    std::string name;
    std::string type;
    std::string file_path;
    int line{};
    int column{};
    std::string signature;
    std::string doc_comment;
};

struct ReferencesRequest {
    std::string pattern;
    int max_results{};
};

struct ReferenceLocation {
    std::string file_path;
    int line{};
    int column{};
    std::string context;
    std::string match_text;
};

struct TreeRequest {
    std::string function_name;
    int max_depth{};
    bool show_lines{};
    bool compact{};
    std::string exclude;
    bool agent_mode{};
};

struct GitAnalyzeRequest {
    std::string scope;
    std::string base_ref;
    std::string target_ref;
    std::vector<std::string> focus;
    double similarity_threshold{};
    int max_findings{};
};

struct ListSymbolsRequest {
    std::string kind;
    std::string file;
    std::optional<bool> exported;
    std::string name;
    std::string receiver;
    std::optional<int> min_complexity;
    std::optional<int> max_complexity;
    std::optional<int> min_params;
    std::optional<int> max_params;
    std::string flags;
    std::string sort;
    int max{};
    int offset{};
    std::string include;
};

struct ListSymbolsEntry {
    std::string name;
    std::string type;
    std::string file;
    int line{};
    std::string object_id;
    bool is_exported{};
    std::string signature;
    int complexity{};
    int parameter_count{};
    std::string receiver_type;
    int incoming_refs{};
    int outgoing_refs{};
    std::vector<std::string> callers;
    std::vector<std::string> callees;
};

struct InspectSymbolRequest {
    std::string name;
    std::string id;
    std::string file;
    std::string type;
    std::string include;
    int max_depth{};
};

struct TypeHierarchyEntry {
    std::vector<std::string> implements;
    std::vector<std::string> implemented_by;
    std::vector<std::string> extends;
    std::vector<std::string> extended_by;

    bool empty() const {
        return implements.empty() && implemented_by.empty() &&
               extends.empty() && extended_by.empty();
    }
};

struct InspectSymbolEntry {
    std::string name;
    std::string object_id;
    std::string type;
    std::string file;
    int line{};
    bool is_exported{};
    std::string signature;
    std::string doc_comment;
    int complexity{};
    int parameter_count{};
    std::string receiver_type;
    std::vector<std::string> function_flags;
    std::vector<std::string> variable_flags;
    std::vector<std::string> callers;
    std::vector<std::string> callees;
    std::optional<TypeHierarchyEntry> type_hierarchy;
    std::vector<std::string> scope_chain;
    int incoming_refs{};
    int outgoing_refs{};
    std::vector<std::string> annotations;
};

struct BrowseFileRequest {
    std::string file;
    std::optional<int> file_id;
    std::string kind;
    std::optional<bool> exported;
    std::string sort;
    int max{};
    std::string include;
    bool show_imports{};
    bool show_stats{};
};

struct BrowseFileInfoEntry {
    std::string path;
    int file_id{};
    std::string language;
};

struct FileStatsEntry {
    int symbol_count{};
    int function_count{};
    int type_count{};
    double avg_complexity{};
    int max_complexity{};
    int exported_count{};
};

struct StatsResponse {
    int file_count{};
    int symbol_count{};
    int64_t index_size_bytes{};
    int64_t build_duration_ms{};
    double memory_rss_mb{};
    int num_threads{};
    double uptime_seconds{};
    int64_t search_count{};
    double avg_search_time_ms{};
    std::string error;
};

// -- IndexServer --------------------------------------------------------------

/// HTTP server on a Unix domain socket providing 15 REST endpoints.
///
/// Wraps a MasterIndex and optional SearchEngine, exposing index queries
/// and lifecycle management over HTTP/JSON.
///
/// Thread safety: All handlers use a shared_mutex for read-heavy access
/// to the indexer and search engine. The server itself is thread-safe
/// via cpp-httplib's internal thread pool.
class IndexServer {
  public:
    /// Creates a server that owns its own index (starts indexing on Start).
    explicit IndexServer(const Config& config);

    /// Creates a server with an externally-managed index and search engine.
    IndexServer(const Config& config,
                MasterIndex& indexer,
                SearchEngine* search_engine);

    ~IndexServer();

    IndexServer(const IndexServer&) = delete;
    IndexServer& operator=(const IndexServer&) = delete;

    /// Sets a custom socket path (for testing).
    void set_socket_path(const std::string& path);

    /// Returns the socket path this server uses.
    std::string socket_path() const;

    /// Sets a build ID override (for testing).
    void set_build_id_override(const std::string& id);

    /// Starts listening on the Unix socket. Returns false on failure.
    bool start();

    /// Blocks until the shutdown signal is received.
    void wait();

    /// Gracefully shuts down and joins all owned threads.
    /// Returns true if shutdown completed cleanly.
    bool shutdown();

    /// Returns true if the server is currently running.
    bool is_running() const;

  private:
    void register_handlers();

    // -- Endpoint handlers ----------------------------------------------------
    void handle_ping(const httplib::Request& req, httplib::Response& res);
    void handle_status(const httplib::Request& req, httplib::Response& res);
    void handle_search(const httplib::Request& req, httplib::Response& res);
    void handle_symbol(const httplib::Request& req, httplib::Response& res);
    void handle_fileinfo(const httplib::Request& req, httplib::Response& res);
    void handle_shutdown(const httplib::Request& req, httplib::Response& res);
    void handle_reindex(const httplib::Request& req, httplib::Response& res);
    void handle_stats(const httplib::Request& req, httplib::Response& res);
    void handle_definition(const httplib::Request& req, httplib::Response& res);
    void handle_references(const httplib::Request& req, httplib::Response& res);
    void handle_tree(const httplib::Request& req, httplib::Response& res);
    void handle_git_analyze(const httplib::Request& req, httplib::Response& res);
    void handle_list_symbols(const httplib::Request& req,
                             httplib::Response& res);
    void handle_inspect_symbol(const httplib::Request& req,
                               httplib::Response& res);
    void handle_browse_file(const httplib::Request& req,
                            httplib::Response& res);

    // -- Helpers --------------------------------------------------------------
    void json_response(httplib::Response& res, const nlohmann::json& body);
    void error_response(httplib::Response& res, int status,
                        const std::string& message);
    bool require_ready(httplib::Response& res);

    std::string language_from_path(const std::string& path) const;

    Config config_;
    std::unique_ptr<MasterIndex> owned_indexer_;
    MasterIndex* indexer_{};
    SearchEngine* search_engine_{};
    std::unique_ptr<SearchEngine> owned_search_engine_;

    httplib::Server svr_;
    std::string socket_path_;
    std::string build_id_override_;

    std::chrono::steady_clock::time_point start_time_;
    std::atomic<bool> running_{false};
    std::atomic<bool> indexing_active_{false};
    mutable std::shared_mutex mu_;
    std::mutex lifecycle_mu_;
    bool handlers_registered_{false};

    std::mutex shutdown_mu_;
    std::condition_variable shutdown_cv_;
    bool shutdown_requested_{false};

    // The /shutdown endpoint defers the actual shutdown by ~100ms (so the HTTP
    // response flushes first). That delay runs on this thread, owned by the
    // server and joined before teardown so it can never outlive the object and
    // touch freed members (shutdown_mu_/shutdown_requested_/shutdown_cv_).
    // `shutdown_triggered_` guards single-spawn across concurrent /shutdown
    // requests. (Previously a detached thread — a use-after-free under fast
    // teardown, TSan-confirmed: see efsw-server-concurrency-races.)
    std::atomic<bool> shutdown_triggered_{false};
    std::thread shutdown_trigger_;

    std::thread listen_thread_;

    // Background indexing thread. Owns the in-flight indexing run
    // (initial start-up index or any /reindex request). Tracked by the
    // server so shutdown can request cooperative cancellation and join
    // before destruction. `indexing_thread_mu_` serialises the
    // cancel/swap/join sequence across concurrent reindex requests and
    // the shutdown path; without serialising the assignment with the
    // cancel, two callers could race and overwrite a joinable thread,
    // which terminates the process.
    std::thread indexing_thread_;
    std::mutex indexing_thread_mu_;

    // Cancels and joins any background indexing thread. Safe to call
    // multiple times. Called from shutdown().
    void cancel_indexing_thread();
    bool shutdown_locked();

    // Cancels any in-flight indexing run, joins its thread, and
    // installs `new_thread` as the active indexing thread. Atomic with
    // respect to other callers; the prior thread is joined under the
    // same lock that gates the swap so concurrent callers serialise
    // through this function rather than racing on `indexing_thread_`.
    void swap_indexing_thread(std::thread new_thread);
};

}  // namespace lci
