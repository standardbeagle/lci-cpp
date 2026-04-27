#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <lci/server/server.h>

namespace lci {

/// HTTP client for communicating with an IndexServer over a Unix domain socket.
///
/// Each method maps 1:1 to a server endpoint. Connection errors and HTTP errors
/// are reported via the returned string (empty on success for void-like calls)
/// or via exceptions for JSON parse failures.
///
/// Thread safety: Not thread-safe. Each thread should use its own Client.
class Client {
  public:
    /// Creates a client using the default socket path.
    Client();

    /// Creates a client with a specific socket path.
    explicit Client(const std::string& socket_path);

    /// Returns true if the server responds to a ping.
    bool is_server_running();

    /// Sends a health check. Returns nullopt on connection failure.
    std::optional<PingResponse> ping(std::string& error);

    /// Retrieves the current index status.
    std::optional<IndexStatus> get_status(std::string& error);

    /// Performs a search query.
    std::optional<nlohmann::json> search(const std::string& pattern,
                                         int max_results,
                                         bool case_insensitive,
                                         bool declaration_only,
                                         std::string& error);

    /// Retrieves symbol information by ID.
    std::optional<nlohmann::json> get_symbol(uint64_t symbol_id,
                                             std::string& error);

    /// Retrieves file information by ID.
    std::optional<nlohmann::json> get_file_info(uint32_t file_id,
                                                std::string& error);

    /// Requests server shutdown.
    bool shutdown(bool force, std::string& error);

    /// Triggers a re-index.
    bool reindex(const std::string& path, std::string& error);

    /// Searches for symbol definitions by name pattern.
    std::optional<std::vector<DefinitionLocation>> get_definition(
        const std::string& pattern, int max_results, std::string& error);

    /// Searches for symbol references by name pattern.
    std::optional<std::vector<ReferenceLocation>> get_references(
        const std::string& pattern, int max_results, std::string& error);

    /// Generates a function call hierarchy tree.
    std::optional<nlohmann::json> get_tree(const TreeRequest& req,
                                           std::string& error);

    /// Retrieves index statistics.
    std::optional<StatsResponse> get_stats(std::string& error);

    /// Waits until the index is ready or timeout expires.
    bool wait_for_ready(std::chrono::milliseconds timeout,
                        std::string& error);

    /// Performs git change analysis.
    std::optional<nlohmann::json> git_analyze(const GitAnalyzeRequest& req,
                                              std::string& error);

    /// Lists symbols matching filters.
    std::optional<nlohmann::json> list_symbols(const ListSymbolsRequest& req,
                                               std::string& error);

    /// Deep-inspects a symbol.
    std::optional<nlohmann::json> inspect_symbol(
        const InspectSymbolRequest& req, std::string& error);

    /// Lists all symbols in a file.
    std::optional<nlohmann::json> browse_file(const BrowseFileRequest& req,
                                              std::string& error);

    /// Sets the connection and read timeout.
    void set_timeout(std::chrono::milliseconds timeout);

  private:
    /// Posts JSON to the given path and returns the parsed response.
    /// On failure, sets error and returns nullopt.
    std::optional<nlohmann::json> post_json(const std::string& path,
                                            const nlohmann::json& body,
                                            std::string& error);

    /// GET request returning parsed JSON.
    std::optional<nlohmann::json> get_json(const std::string& path,
                                           std::string& error);

    std::string socket_path_;
    std::chrono::milliseconds timeout_{30000};
};

}  // namespace lci
