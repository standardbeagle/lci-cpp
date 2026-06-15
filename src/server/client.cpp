#include <lci/server/client.h>

#include <httplib.h>

#include <thread>

namespace lci {

Client::Client() : socket_path_(get_socket_path()) {}

Client::Client(const std::string& socket_path) : socket_path_(socket_path) {}

bool Client::is_server_running() {
    std::string err;
    auto resp = ping(err);
    return resp.has_value();
}

std::optional<PingResponse> Client::ping(std::string& error) {
    auto j = post_json("/ping", nlohmann::json{}, error);
    if (!j) return std::nullopt;

    PingResponse resp;
    resp.uptime_seconds = j->value("uptime_seconds", 0.0);
    resp.version = j->value("version", "");
    resp.build_id_value = j->value("build_id", "");
    return resp;
}

std::optional<IndexStatus> Client::get_status(std::string& error) {
    auto j = get_json("/status", error);
    if (!j) return std::nullopt;

    IndexStatus status;
    status.ready = j->value("ready", false);
    status.file_count = j->value("file_count", 0);
    status.symbol_count = j->value("symbol_count", 0);
    status.indexing_active = j->value("indexing_active", false);
    status.progress = j->value("progress", 0.0);
    status.error = j->value("error", "");
    return status;
}

std::optional<nlohmann::json> Client::search(const std::string& pattern,
                                             int max_results,
                                             bool case_insensitive,
                                             bool declaration_only,
                                             std::string& error) {
    nlohmann::json body = {
        {"pattern", pattern},
        {"max_results", max_results},
        {"case_insensitive", case_insensitive},
        {"declaration_only", declaration_only}};
    auto j = post_json("/search", body, error);
    if (!j) return std::nullopt;

    if (j->contains("error") && !(*j)["error"].get<std::string>().empty()) {
        error = "search error: " + (*j)["error"].get<std::string>();
        return std::nullopt;
    }
    return j;
}

std::optional<nlohmann::json> Client::get_symbol(uint64_t symbol_id,
                                                 std::string& error) {
    nlohmann::json body = {{"symbol_id", symbol_id}};
    auto j = post_json("/symbol", body, error);
    if (!j) return std::nullopt;

    if (j->contains("error") && !(*j)["error"].get<std::string>().empty()) {
        error = "symbol error: " + (*j)["error"].get<std::string>();
        return std::nullopt;
    }
    return j;
}

std::optional<nlohmann::json> Client::get_file_info(uint32_t file_id,
                                                    std::string& error) {
    nlohmann::json body = {{"file_id", file_id}};
    auto j = post_json("/fileinfo", body, error);
    if (!j) return std::nullopt;

    if (j->contains("error") && !(*j)["error"].get<std::string>().empty()) {
        error = "file info error: " + (*j)["error"].get<std::string>();
        return std::nullopt;
    }
    return j;
}

bool Client::shutdown(bool force, std::string& error) {
    nlohmann::json body = {{"force", force}};
    auto j = post_json("/shutdown", body, error);
    if (!j) return false;

    if (!j->value("success", false)) {
        error = "shutdown failed: " + j->value("message", "unknown");
        return false;
    }
    return true;
}

bool Client::reindex(const std::string& path, std::string& error) {
    nlohmann::json body = {{"path", path}};
    auto j = post_json("/reindex", body, error);
    if (!j) return false;

    if (!j->value("success", false)) {
        error = "reindex failed: " + j->value("message", "unknown");
        return false;
    }
    return true;
}

std::optional<std::vector<DefinitionLocation>> Client::get_definition(
    const std::string& pattern, int max_results, std::string& error) {
    nlohmann::json body = {{"pattern", pattern}, {"max_results", max_results}};
    auto j = post_json("/definition", body, error);
    if (!j) return std::nullopt;

    if (j->contains("error") && !(*j)["error"].get<std::string>().empty()) {
        error = "definition error: " + (*j)["error"].get<std::string>();
        return std::nullopt;
    }

    std::vector<DefinitionLocation> defs;
    if (j->contains("definitions") && (*j)["definitions"].is_array()) {
        for (const auto& d : (*j)["definitions"]) {
            DefinitionLocation loc;
            loc.name = d.value("name", "");
            loc.type = d.value("type", "");
            loc.file_path = d.value("file_path", "");
            loc.line = d.value("line", 0);
            loc.column = d.value("column", 0);
            loc.signature = d.value("signature", "");
            loc.doc_comment = d.value("doc_comment", "");
            defs.push_back(std::move(loc));
        }
    }
    return defs;
}

std::optional<std::vector<ReferenceLocation>> Client::get_references(
    const std::string& pattern, int max_results, std::string& error) {
    nlohmann::json body = {{"pattern", pattern}, {"max_results", max_results}};
    auto j = post_json("/references", body, error);
    if (!j) return std::nullopt;

    if (j->contains("error") && !(*j)["error"].get<std::string>().empty()) {
        error = "references error: " + (*j)["error"].get<std::string>();
        return std::nullopt;
    }

    std::vector<ReferenceLocation> refs;
    if (j->contains("references") && (*j)["references"].is_array()) {
        for (const auto& r : (*j)["references"]) {
            ReferenceLocation loc;
            loc.file_path = r.value("file_path", "");
            loc.line = r.value("line", 0);
            loc.column = r.value("column", 0);
            loc.context = r.value("context", "");
            loc.match_text = r.value("match_text", "");
            refs.push_back(std::move(loc));
        }
    }
    return refs;
}

std::optional<nlohmann::json> Client::get_tree(const TreeRequest& req,
                                               std::string& error) {
    nlohmann::json body = {{"function_name", req.function_name},
                           {"max_depth", req.max_depth},
                           {"show_lines", req.show_lines},
                           {"compact", req.compact},
                           {"exclude", req.exclude},
                           {"agent_mode", req.agent_mode}};
    auto j = post_json("/tree", body, error);
    if (!j) return std::nullopt;

    if (j->contains("error") && !(*j)["error"].get<std::string>().empty()) {
        error = "tree error: " + (*j)["error"].get<std::string>();
        return std::nullopt;
    }
    return j;
}

std::optional<StatsResponse> Client::get_stats(std::string& error) {
    auto j = post_json("/stats", nlohmann::json{}, error);
    if (!j) return std::nullopt;

    if (j->contains("error") && !(*j)["error"].get<std::string>().empty()) {
        error = "stats error: " + (*j)["error"].get<std::string>();
        return std::nullopt;
    }

    StatsResponse stats;
    stats.file_count = j->value("file_count", 0);
    stats.symbol_count = j->value("symbol_count", 0);
    stats.index_size_bytes = j->value("index_size_bytes", int64_t{0});
    stats.build_duration_ms = j->value("build_duration_ms", int64_t{0});
    stats.memory_rss_mb = j->value("memory_rss_mb", 0.0);
    stats.num_threads = j->value("num_threads", 0);
    stats.uptime_seconds = j->value("uptime_seconds", 0.0);
    stats.search_count = j->value("search_count", int64_t{0});
    stats.avg_search_time_ms = j->value("avg_search_time_ms", 0.0);
    return stats;
}

bool Client::wait_for_ready(std::chrono::milliseconds timeout,
                            std::string& error) {
    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        std::string err;
        auto status = get_status(err);
        if (status && status->ready) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    error = "timeout waiting for index to be ready";
    return false;
}

std::optional<nlohmann::json> Client::git_analyze(
    const GitAnalyzeRequest& req, std::string& error) {
    nlohmann::json body = {{"scope", req.scope}};
    if (!req.base_ref.empty()) body["base_ref"] = req.base_ref;
    if (!req.target_ref.empty()) body["target_ref"] = req.target_ref;
    if (!req.focus.empty()) body["focus"] = req.focus;
    if (req.similarity_threshold > 0.0) {
        body["similarity_threshold"] = req.similarity_threshold;
    }
    if (req.max_findings > 0) body["max_findings"] = req.max_findings;

    auto j = post_json("/git-analyze", body, error);
    if (!j) return std::nullopt;

    if (j->contains("error") && !(*j)["error"].get<std::string>().empty()) {
        error = "git analyze error: " + (*j)["error"].get<std::string>();
        return std::nullopt;
    }
    return j;
}

std::optional<nlohmann::json> Client::list_symbols(
    const ListSymbolsRequest& req, std::string& error) {
    nlohmann::json body = {{"kind", req.kind}};
    if (!req.file.empty()) body["file"] = req.file;
    if (req.exported.has_value()) body["exported"] = *req.exported;
    if (!req.name.empty()) body["name"] = req.name;
    if (!req.receiver.empty()) body["receiver"] = req.receiver;
    if (req.min_complexity.has_value()) {
        body["min_complexity"] = *req.min_complexity;
    }
    if (req.max_complexity.has_value()) {
        body["max_complexity"] = *req.max_complexity;
    }
    if (req.min_params.has_value()) body["min_params"] = *req.min_params;
    if (req.max_params.has_value()) body["max_params"] = *req.max_params;
    if (!req.flags.empty()) body["flags"] = req.flags;
    if (!req.sort.empty()) body["sort"] = req.sort;
    if (req.max > 0) body["max"] = req.max;
    if (req.offset > 0) body["offset"] = req.offset;
    if (!req.include.empty()) body["include"] = req.include;

    auto j = post_json("/list-symbols", body, error);
    if (!j) return std::nullopt;

    if (j->contains("error") && !(*j)["error"].get<std::string>().empty()) {
        error = "list symbols error: " + (*j)["error"].get<std::string>();
        return std::nullopt;
    }
    return j;
}

std::optional<nlohmann::json> Client::inspect_symbol(
    const InspectSymbolRequest& req, std::string& error) {
    nlohmann::json body;
    if (!req.name.empty()) body["name"] = req.name;
    if (!req.id.empty()) body["id"] = req.id;
    if (!req.file.empty()) body["file"] = req.file;
    if (!req.type.empty()) body["type"] = req.type;
    if (!req.include.empty()) body["include"] = req.include;
    if (req.max_depth > 0) body["max_depth"] = req.max_depth;

    auto j = post_json("/inspect-symbol", body, error);
    if (!j) return std::nullopt;

    if (j->contains("error") && !(*j)["error"].get<std::string>().empty()) {
        error = "inspect symbol error: " + (*j)["error"].get<std::string>();
        return std::nullopt;
    }
    return j;
}

std::optional<nlohmann::json> Client::browse_file(
    const BrowseFileRequest& req, std::string& error) {
    nlohmann::json body;
    if (!req.file.empty()) body["file"] = req.file;
    if (req.file_id.has_value()) body["file_id"] = *req.file_id;
    if (!req.kind.empty()) body["kind"] = req.kind;
    if (req.exported.has_value()) body["exported"] = *req.exported;
    if (!req.sort.empty()) body["sort"] = req.sort;
    if (req.max > 0) body["max"] = req.max;
    if (!req.include.empty()) body["include"] = req.include;
    if (req.show_imports) body["show_imports"] = true;
    if (req.show_stats) body["show_stats"] = true;

    auto j = post_json("/browse-file", body, error);
    if (!j) return std::nullopt;

    if (j->contains("error") && !(*j)["error"].get<std::string>().empty()) {
        error = "browse file error: " + (*j)["error"].get<std::string>();
        return std::nullopt;
    }
    return j;
}

void Client::set_timeout(std::chrono::milliseconds timeout) {
    timeout_ = timeout;
}

std::optional<nlohmann::json> Client::post_json(const std::string& path,
                                                const nlohmann::json& body,
                                                std::string& error) {
#ifdef _WIN32
    // On Windows, socket_path_ is "127.0.0.1:<port>" for TCP transport.
    httplib::Client cli("http://" + socket_path_);
#else
    httplib::Client cli(socket_path_);
    cli.set_address_family(AF_UNIX);
#endif
    cli.set_connection_timeout(timeout_);
    cli.set_read_timeout(timeout_);

    auto res = cli.Post(path, body.dump(), "application/json");
    if (!res) {
        error = "failed to connect to server at " + socket_path_ + ": " +
                httplib::to_string(res.error());
        return std::nullopt;
    }

    if (res->status != 200) {
        error = "server error (HTTP " + std::to_string(res->status) +
                "): " + res->body;
        return std::nullopt;
    }

    try {
        return nlohmann::json::parse(res->body);
    } catch (const nlohmann::json::parse_error& e) {
        error = std::string("failed to decode response: ") + e.what();
        return std::nullopt;
    }
}

std::optional<nlohmann::json> Client::get_json(const std::string& path,
                                               std::string& error) {
#ifdef _WIN32
    httplib::Client cli("http://" + socket_path_);
#else
    httplib::Client cli(socket_path_);
    cli.set_address_family(AF_UNIX);
#endif
    cli.set_connection_timeout(timeout_);
    cli.set_read_timeout(timeout_);

    auto res = cli.Get(path);
    if (!res) {
        error = "failed to connect to server at " + socket_path_ + ": " +
                httplib::to_string(res.error());
        return std::nullopt;
    }

    if (res->status != 200) {
        error = "server error (HTTP " + std::to_string(res->status) +
                "): " + res->body;
        return std::nullopt;
    }

    try {
        return nlohmann::json::parse(res->body);
    } catch (const nlohmann::json::parse_error& e) {
        error = std::string("failed to decode response: ") + e.what();
        return std::nullopt;
    }
}

}  // namespace lci
