#include <lci/mcp/handlers_index.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include <lci/core/file_content_store.h>
#include <lci/core/reference_tracker.h>
#include <lci/indexing/master_index.h>
#include <lci/symbol.h>

namespace lci {
namespace mcp {

// -- Helpers ------------------------------------------------------------------

namespace {

/// Converts a string to lowercase.
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

/// Clamps an integer to a range.
int clamp_int(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

/// Guesses programming language from file extension.
std::string language_from_path(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "unknown";
    auto ext = to_lower(path.substr(dot));
    if (ext == ".go") return "go";
    if (ext == ".js") return "javascript";
    if (ext == ".ts") return "typescript";
    if (ext == ".jsx") return "jsx";
    if (ext == ".tsx") return "tsx";
    if (ext == ".py") return "python";
    if (ext == ".rs") return "rust";
    if (ext == ".java") return "java";
    if (ext == ".cs") return "csharp";
    if (ext == ".kt") return "kotlin";
    if (ext == ".rb") return "ruby";
    if (ext == ".swift") return "swift";
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") return "cpp";
    if (ext == ".c") return "c";
    if (ext == ".h" || ext == ".hpp") return "c/cpp";
    return "unknown";
}

/// Formats a system_clock time_point as RFC3339Nano with local timezone offset
/// (matches Go's time.Time JSON marshal: "2026-05-13T17:12:53.528955893-05:00").
/// Zero-alloc on hot path: writes into a fixed 48-byte stack buffer then a
/// single std::string move out.
std::string format_rfc3339_nano_local(
    std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto secs = time_point_cast<seconds>(tp);
    auto ns = duration_cast<nanoseconds>(tp - secs).count();
    std::time_t t = system_clock::to_time_t(secs);
    std::tm tm{};
    localtime_r(&t, &tm);

    // Compute local zone offset from UTC.
    std::tm utm{};
    gmtime_r(&t, &utm);
    // Difference in seconds: treat both tms as if UTC for difftime.
    std::time_t lt = std::mktime(&tm);
    std::time_t ut = std::mktime(&utm);
    long offset = static_cast<long>(lt - ut);
    char sign = offset < 0 ? '-' : '+';
    long abs_off = offset < 0 ? -offset : offset;
    int oh = static_cast<int>(abs_off / 3600);
    int om = static_cast<int>((abs_off % 3600) / 60);

    char buf[48];
    int n = std::snprintf(buf, sizeof(buf),
                          "%04d-%02d-%02dT%02d:%02d:%02d.%09ld%c%02d:%02d",
                          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                          tm.tm_hour, tm.tm_min, tm.tm_sec,
                          static_cast<long>(ns), sign, oh, om);
    if (n <= 0) return std::string{};
    return std::string(buf, static_cast<size_t>(n));
}

/// Computes total bytes across all indexed file contents.
int64_t compute_total_size_bytes(MasterIndex& indexer) {
    int64_t total = 0;
    const auto& store = indexer.file_content_store();
    for (auto fid : indexer.get_all_file_ids()) {
        auto sv = store.get_content(fid);
        total += static_cast<int64_t>(sv.size());
    }
    return total;
}

/// Builds the symbol type distribution map.
nlohmann::json get_symbols_by_type(ReferenceTracker& tracker,
                                   MasterIndex& indexer) {
    nlohmann::json result = nlohmann::json::object();
    auto file_ids = indexer.get_all_file_ids();
    for (auto fid : file_ids) {
        auto symbols = tracker.get_file_enhanced_symbols(fid);
        for (const auto* sym : symbols) {
            if (!sym) continue;
            auto type_name = std::string(to_string(sym->symbol.type));
            if (result.contains(type_name)) {
                result[type_name] = result[type_name].get<int>() + 1;
            } else {
                result[type_name] = 1;
            }
        }
    }
    return result;
}

/// Builds the language distribution map.
nlohmann::json get_files_by_language(MasterIndex& indexer) {
    nlohmann::json result = nlohmann::json::object();
    auto file_ids = indexer.get_all_file_ids();
    for (auto fid : file_ids) {
        auto path = indexer.get_file_path(fid);
        if (path.empty()) continue;
        auto lang = language_from_path(path);
        if (result.contains(lang)) {
            result[lang] = result[lang].get<int>() + 1;
        } else {
            result[lang] = 1;
        }
    }
    return result;
}

}  // namespace

// -- handle_index_stats -------------------------------------------------------

ToolResult handle_index_stats(const nlohmann::json& params,
                              MasterIndex& indexer) {
    auto mode = to_lower(params.value("mode", "summary"));
    bool include_memory = params.value("include_memory", false);
    bool include_components = params.value("include_components", false);

    auto stats = indexer.get_stats();

    auto now = std::chrono::system_clock::now();

    nlohmann::json response;
    // Match Go shape: ISO8601 string (RFC3339Nano with local zone) under
    // `timestamp` key. Previously emitted `timestamp_ms` int — fixed in
    // iter-8 (DART-vJrTKPxyk7RF) to align with Go's time.Time JSON marshal.
    response["timestamp"] = format_rfc3339_nano_local(now);
    response["server_ready"] = true;

    if (stats.is_indexing) {
        response["status"] = "indexing";
        nlohmann::json progress;
        progress["is_indexing"] = true;
        progress["total_files"] = static_cast<int>(stats.total_files_to_process);
        progress["files_processed"] = static_cast<int>(stats.processed_files);
        if (stats.total_files_to_process > 0) {
            double pct = static_cast<double>(stats.processed_files) /
                         static_cast<double>(stats.total_files_to_process) *
                         100.0;
            progress["overall_progress"] = pct;
        }
        response["progress"] = std::move(progress);
    } else {
        response["status"] = "ready";
    }

    response["file_count"] = stats.total_files;
    response["symbol_count"] = stats.total_symbols;
    response["reference_count"] = stats.total_references;
    // Match Go shape: sum of all indexed file content sizes. Previously
    // omitted — fixed in iter-8 (DART-vJrTKPxyk7RF) using FileContentStore.
    response["total_size_bytes"] = compute_total_size_bytes(indexer);
    response["index_time_ms"] = stats.indexing_time_ns / 1000000;

    // Component health for detailed/health modes
    if (mode == "detailed" || mode == "health" || include_components) {
        auto& tracker = indexer.ref_tracker();
        auto ref_stats = tracker.get_reference_stats();
        nlohmann::json health;
        health["symbol_index_ready"] = true;
        health["trigram_index_ready"] = true;
        health["ref_tracker_ready"] = true;
        health["call_graph_populated"] = tracker.has_relationships();
        health["file_content_store_ready"] = true;
        health["reference_stats"]["total_symbols"] = ref_stats.total_symbols;
        health["reference_stats"]["total_references"] =
            ref_stats.total_references;
        response["component_health"] = std::move(health);

        // Collect warnings
        nlohmann::json warnings = nlohmann::json::array();
        if (!tracker.has_relationships()) {
            warnings.push_back(
                "Call graph is empty - relationship queries may return no data");
        }
        if (!warnings.empty()) {
            response["warnings"] = std::move(warnings);
        }
    }

    // Memory info (C++ uses process memory, not Go runtime stats)
    if (mode == "detailed" || include_memory) {
        nlohmann::json mem;
        mem["note"] =
            "C++ process memory stats not directly comparable to Go runtime";
        response["memory_usage"] = std::move(mem);
    }

    return make_json_response(response);
}

// -- handle_debug_info --------------------------------------------------------

ToolResult handle_debug_info(const nlohmann::json& params,
                             MasterIndex& indexer) {
    auto mode = to_lower(params.value("mode", "overview"));
    int max_results = clamp_int(params.value("max_results", 20), 1, 100);

    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch())
                      .count();

    auto& tracker = indexer.ref_tracker();

    nlohmann::json response;
    response["mode"] = mode;
    response["timestamp_ms"] = now_ms;

    if (mode == "overview") {
        auto stats = indexer.get_stats();
        auto lang_breakdown = get_files_by_language(indexer);
        auto type_breakdown = get_symbols_by_type(tracker, indexer);

        double avg_symbols = 0.0;
        if (stats.total_files > 0) {
            avg_symbols = static_cast<double>(stats.total_symbols) /
                          static_cast<double>(stats.total_files);
        }
        double avg_refs = 0.0;
        if (stats.total_symbols > 0) {
            avg_refs = static_cast<double>(stats.total_references) /
                       static_cast<double>(stats.total_symbols);
        }

        nlohmann::json overview;
        overview["total_files"] = stats.total_files;
        overview["total_symbols"] = stats.total_symbols;
        overview["total_references"] = stats.total_references;
        overview["unique_languages"] = static_cast<int>(lang_breakdown.size());
        overview["avg_symbols_per_file"] = avg_symbols;
        overview["avg_refs_per_symbol"] = avg_refs;
        overview["language_breakdown"] = std::move(lang_breakdown);
        overview["type_breakdown"] = std::move(type_breakdown);
        response["overview"] = std::move(overview);
    } else if (mode == "symbols" || mode == "types") {
        response["symbols_by_type"] = get_symbols_by_type(tracker, indexer);
    } else if (mode == "references") {
        // Get top referenced symbols
        auto& sym_store = indexer.ref_tracker();
        auto file_ids = indexer.get_all_file_ids();

        struct SymRef {
            std::string name;
            std::string type;
            std::string path;
            int incoming;
        };
        std::vector<SymRef> all_refs;

        for (auto fid : file_ids) {
            auto fp = indexer.get_file_path(fid);
            auto symbols = sym_store.get_file_enhanced_symbols(fid);
            for (const auto* sym : symbols) {
                if (!sym) continue;
                int inc = static_cast<int>(sym->incoming_refs.size());
                if (inc > 0) {
                    all_refs.push_back({sym->symbol.name,
                                        std::string(to_string(sym->symbol.type)),
                                        fp, inc});
                }
            }
        }

        std::sort(all_refs.begin(), all_refs.end(),
                  [](const SymRef& a, const SymRef& b) {
                      return a.incoming > b.incoming;
                  });

        if (static_cast<int>(all_refs.size()) > max_results) {
            all_refs.resize(static_cast<size_t>(max_results));
        }

        nlohmann::json top = nlohmann::json::array();
        for (const auto& sr : all_refs) {
            nlohmann::json item;
            item["symbol_name"] = sr.name;
            item["symbol_type"] = sr.type;
            item["file_path"] = sr.path;
            item["incoming_refs"] = sr.incoming;
            top.push_back(std::move(item));
        }
        response["top_referenced_symbols"] = std::move(top);
    } else if (mode == "files") {
        response["files_by_language"] = get_files_by_language(indexer);

        // Optional specific file info
        int file_id_param = params.value("file_id", 0);
        auto file_path_param = params.value("file_path", "");
        bool verbose = params.value("verbose", false);

        FileID target_fid = 0;
        if (file_id_param > 0) {
            target_fid = static_cast<FileID>(file_id_param);
        } else if (!file_path_param.empty()) {
            target_fid = indexer.path_to_id(file_path_param);
        }

        if (target_fid != 0) {
            auto fp = indexer.get_file_path(target_fid);
            if (!fp.empty()) {
                auto symbols = tracker.get_file_enhanced_symbols(target_fid);
                nlohmann::json fi;
                fi["file_id"] = static_cast<int>(target_fid);
                fi["file_path"] = fp;
                fi["language"] = language_from_path(fp);
                fi["symbol_count"] = static_cast<int>(symbols.size());

                if (verbose) {
                    nlohmann::json sym_list = nlohmann::json::array();
                    for (const auto* sym : symbols) {
                        if (!sym) continue;
                        nlohmann::json s;
                        s["name"] = sym->symbol.name;
                        s["type"] = std::string(to_string(sym->symbol.type));
                        s["line"] = sym->symbol.line;
                        s["is_exported"] = sym->is_exported;
                        s["incoming_refs"] =
                            static_cast<int>(sym->incoming_refs.size());
                        s["outgoing_refs"] =
                            static_cast<int>(sym->outgoing_refs.size());
                        sym_list.push_back(std::move(s));
                    }
                    fi["symbols"] = std::move(sym_list);
                }
                response["file_info"] = std::move(fi);
            }
        }
    } else {
        return make_error_response(
            "debug_info", "unknown mode: " + mode +
                              " (use overview, symbols, references, types, "
                              "or files)");
    }

    return make_json_response(response);
}

// -- handle_git_analysis ------------------------------------------------------

ToolResult handle_git_analysis(const nlohmann::json& /*params*/) {
    nlohmann::json response;
    response["status"] = "not_available";
    response["message"] =
        "Git analysis engine has not been ported to C++ yet. "
        "This feature will be available in a future release.";
    return make_json_response(response);
}

// -- register_index_handlers --------------------------------------------------

void register_index_handlers(McpServer& server, MasterIndex* indexer) {
    server.add_tool(
        {"index_stats",
         "Index status and health monitoring. Shows file counts, symbol "
         "counts, indexing progress, and component health.",
         {{"mode", "string",
           "Query mode: summary (default), detailed, progress, health", ""},
          {"include_memory", "boolean", "Include memory usage info", ""},
          {"include_components", "boolean",
           "Include component health details", ""}},
         {}},
        [indexer](const nlohmann::json& p) -> ToolResult {
            if (!indexer) {
                return make_error_response("index_stats",
                                           "index not available");
            }
            return handle_index_stats(p, *indexer);
        });

    server.add_tool(
        {"debug_info",
         "Diagnostic information about indexed data. Modes: overview, "
         "symbols, references, types, files.",
         {{"mode", "string",
           "Query mode: overview (default), symbols, references, types, "
           "files",
           ""},
          {"file_id", "integer", "File ID for file-specific info", ""},
          {"file_path", "string", "File path for file-specific info", ""},
          {"max_results", "integer",
           "Max results for lists (default 20)", ""},
          {"verbose", "boolean", "Include detailed symbol info", ""}},
         {}},
        [indexer](const nlohmann::json& p) -> ToolResult {
            if (!indexer) {
                return make_error_response("debug_info",
                                           "index not available");
            }
            return handle_debug_info(p, *indexer);
        });

    server.add_tool(
        {"git_analysis",
         "Analyze git changes for code quality issues (duplicates, naming, "
         "metrics). Currently a stub - git engine not yet ported.",
         {{"scope", "string",
           "Analysis scope: staged, wip, commit, range", ""},
          {"base_ref", "string", "Base reference for commit/range", ""},
          {"target_ref", "string", "Target reference for range", ""},
          {"focus", "array", "Focus areas: duplicates, naming, metrics",
           "string"},
          {"similarity_threshold", "number",
           "Similarity threshold for duplicates", ""},
          {"max_findings", "integer", "Maximum findings to return", ""}},
         {}},
        [](const nlohmann::json& p) -> ToolResult {
            return handle_git_analysis(p);
        });
}

}  // namespace mcp
}  // namespace lci
