#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
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

// -- Instance registry --------------------------------------------------------

/// One live server, as published in the per-user instance registry by
/// IndexServer::enable_instance_registry.
struct ServerInstance {
    uint32_t pid{};
    std::string address;     // socket path (POSIX) or host:port (Windows)
    std::string root;        // project root this server indexes
    std::string entry_path;  // registry file backing this entry
    std::filesystem::file_time_type last_activity{};
    bool root_exists{};      // false once the indexed root is deleted
};

/// Enumerates this user's live servers registered under `dir`, ordered
/// least-recently-active first (the eviction order).
///
/// A server that no longer answers /ping is not reported and its registry
/// entry is removed: an entry outliving its process is litter, not a server.
/// `exclude_entry`, when non-empty, skips one entry path so a caller can omit
/// itself.
std::vector<ServerInstance> list_server_instances(
    const std::string& dir, const std::string& exclude_entry = {});

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
    // Live in-flight counters from the server's indexing_progress block.
    // file_count/progress stay 0 until the index is ready, so these are the
    // only fields that advance during a long initial index — stall detection
    // must key on them.
    int files_scanned{};
    int percent_complete{};
    std::string phase;
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

    /// Opts this server into the cross-process instance registry rooted at
    /// `dir`: on start it publishes a registry file (mtime = last activity)
    /// and evicts least-recently-active peer servers beyond
    /// config.server.max_instances. Off by default so embedded/test servers
    /// never touch — or evict — the user's real servers in the system temp
    /// dir; the CLI server path enables it with the system temp dir.
    void enable_instance_registry(const std::string& dir);

    /// Publishes (or clears, with nullptr) an externally-built SearchEngine
    /// on a server constructed with an externally-managed index. Until an
    /// engine is published the server answers 503 "still indexing"; /status
    /// reports the external build via indexing_active. The engine must
    /// outlive the server or be cleared before it is destroyed.
    void set_search_engine(SearchEngine* engine);

    /// Installs the MCP dispatcher behind POST /mcp: one JSON-RPC message
    /// in, one wire response line out ("" = notification, answered 204).
    /// Unset ⇒ /mcp answers 501, which `lci mcp` reads as "this server
    /// cannot host MCP; run in-process". The dispatcher is responsible for
    /// its own readiness blocking and serialization (McpServer::
    /// dispatch_wire provides both).
    void set_mcp_dispatcher(std::function<std::string(const std::string&)> d);

    /// The index this server serves (owned or externally managed). Stable
    /// for the server's lifetime; used by run_server to build the MCP
    /// runtime over the same index it indexes.
    MasterIndex& index() { return *indexer_; }

    /// True once the index is built and the search engine is published
    /// (the same condition /status reports as ready).
    bool is_ready() const {
        std::shared_lock lock(mu_);
        return search_engine_ != nullptr &&
               !indexing_active_.load(std::memory_order_acquire);
    }

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

    // Inode of the Unix socket this server bound (0 = none/unknown).
    // Teardown unlinks the socket path only while it still holds this
    // inode, so a successor server that rebound the same path during a
    // restart race is never orphaned by our unlink.
    std::atomic<uint64_t> bound_socket_ino_{0};

    // flock fd claiming the socket path for this server's lifetime (-1 =
    // unclaimed / Windows). Taken before probe+bind in start(), released
    // in shutdown_locked(); the kernel releases it on process death.
    int socket_lock_fd_{-1};

    // -- Lifecycle reaper -----------------------------------------------------
    // One background thread per server enforcing three policies:
    //   1. idle exit: no non-/ping request for server.idle_timeout_sec;
    //   2. root-gone exit: the project root existed at start() and has since
    //      been deleted (the leak class: index a temp dir, delete it, server
    //      lives forever);
    //   3. instance eviction (registry enabled only): on startup, dead peer
    //      registry entries are reaped and least-recently-active live peers
    //      beyond server.max_instances are asked to /shutdown.
    // The reaper never calls shutdown_locked() (it is joined there); it exits
    // the process's serve loop by clearing running_ and signalling
    // shutdown_cv_, and the owner completes teardown via shutdown().
    std::thread reaper_thread_;
    std::atomic<int64_t> last_activity_ns_{0};
    std::atomic<int64_t> last_registry_touch_ns_{0};
    std::string registry_dir_;   // empty = registry/eviction disabled
    std::string registry_path_;  // this server's registry file, once published

    // POST /mcp dispatcher (see set_mcp_dispatcher). Set before start().
    std::function<std::string(const std::string&)> mcp_dispatcher_;

    void reaper_loop(bool root_existed_at_start);
    void touch_activity();
    void publish_registry_entry();
    bool write_registry_file();
    void evict_excess_peers();
    void request_self_stop(const char* reason);

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
