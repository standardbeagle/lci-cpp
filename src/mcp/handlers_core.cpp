#include <lci/mcp/handlers_core.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <lci/core/reference_tracker.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/pagination.h>
#include <lci/mcp/validation.h>
#include <lci/search/search_engine.h>
#include <lci/search/search_options.h>
#include <lci/version.h>  // generated: lci::kVersion

namespace lci {
namespace mcp {

// -- Helpers ------------------------------------------------------------------

namespace {

/// Returns true if the comma-separated list contains the given item.
bool comma_list_contains(const std::string& list, const std::string& item) {
    if (list.empty()) return false;
    size_t start = 0;
    while (start < list.size()) {
        auto end = list.find(',', start);
        if (end == std::string::npos) end = list.size();
        auto token = list.substr(start, end - start);
        auto first = token.find_first_not_of(' ');
        if (first != std::string::npos) {
            auto last = token.find_last_not_of(' ');
            if (token.substr(first, last - first + 1) == item) return true;
        }
        start = end + 1;
    }
    return false;
}

/// Parses output format and returns context line count.
int parse_context_lines(const std::string& output) {
    if (output == "full") return 10;
    if (output == "ctx") return 5;
    if (output.size() > 4 && output.substr(0, 4) == "ctx:") {
        try {
            return std::stoi(output.substr(4));
        } catch (...) {
            return 5;
        }
    }
    return 1;
}

/// Clamps an integer to a range.
int clamp_int(int value, int min_val, int max_val) {
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

/// Converts a string to lowercase.
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

}  // namespace

// -- handle_info --------------------------------------------------------------

ToolResult handle_info(const nlohmann::json& params) {
    auto tool = to_lower(params.value("tool", ""));

    if (tool == "version") {
        nlohmann::json data;
        data["name"] = "version";
        data["server_name"] = "lightning-code-index-mcp";
        data["server_version"] = kVersion;
        data["mcp_version"] = "2025-06-18";
        data["capabilities"] = {"stdio_transport", "semantic_search",
                                "regex_search", "symbol_analysis",
                                "call_hierarchy", "multi_language_support"};
        return make_json_response(data);
    }

    if (tool == "search") {
        nlohmann::json data;
        data["name"] = "search";
        data["description"] =
            "Sub-millisecond semantic code search with multi-layer matching.";
        data["parameters"] = {
            {"pattern", "REQUIRED: Search pattern (string)"},
            {"max", "Max results (default: 50, hard cap: 100)"},
            {"output",
             "Output format: 'line', 'ctx', 'ctx:N', 'full', 'files', "
             "'count'"},
            {"filter", "File filter: 'go,*.py' (types/patterns)"},
            {"flags",
             "Search flags: 'ci' (case-insensitive), 'rx' (regex), 'iv' "
             "(invert), 'wb' (word-boundary), 'nt' (no-tests), 'nc' "
             "(no-comments)"},
        };
        data["example"] = {{"basic", R"({"pattern": "user"})"},
                           {"with_flags", R"({"pattern": "user", "flags": "ci,nt"})"},
                           {"with_output", R"({"pattern": "TODO", "output": "ctx:3"})"}};
        return make_json_response(data);
    }

    if (tool == "get_context") {
        nlohmann::json data;
        data["name"] = "get_context";
        data["description"] =
            "Deep context retrieval for symbols. Get call hierarchy and "
            "references.";
        data["parameters"] = {
            {"name", "Symbol name for lookup"},
            {"file_id", "File ID to narrow scope"},
            {"line", "Line number within file"},
            {"include_call_hierarchy", "Include call graph (default: true)"},
            {"max_depth", "Max depth for call hierarchy (default: 3)"},
        };
        data["examples"] = {{"by_name", R"({"name": "handleSearch"})"},
                            {"with_hierarchy",
                             R"({"name": "handleSearch", "include_call_hierarchy": true})"}};
        return make_json_response(data);
    }

    if (tool == "find_files" || tool == "files") {
        nlohmann::json data;
        data["name"] = "find_files";
        data["description"] =
            "Like 'find' or 'fd' - searches file paths on an in-memory index.";
        data["parameters"] = {
            {"pattern", "REQUIRED: File/path pattern to search for"},
            {"max", "Maximum results (default: 50, max: 200)"},
            {"filter", "Filter by file type or glob"},
            {"flags", "Search flags: 'ci' (case-insensitive), 'exact'"},
            {"include_hidden", "Include hidden files (default: false)"},
            {"directory", "Directory to search within"},
        };
        data["examples"] = {
            {"by_name", R"({"pattern": "UserController"})"},
            {"with_filter", R"({"pattern": "handler", "filter": "*.go"})"}};
        return make_json_response(data);
    }

    if (tool == "index_stats") {
        nlohmann::json data;
        data["name"] = "index_stats";
        data["description"] =
            "Index status and health monitoring for diagnostics.";
        data["parameters"] = {
            {"mode", "Query mode: 'summary', 'detailed', 'progress', 'health'"},
        };
        return make_json_response(data);
    }

    // Default: overview of all tools.
    // Output shape matches Go's `internal/mcp` info handler verbatim — the
    // descriptor at tests/parity/descriptors/mcp/info/basic.parity.json
    // compares the full text of result.content[0].text against Go's canon.
    // Field-by-field changes here must be mirrored in the Go reference.
    nlohmann::json data;
    data["available_tools"] = {
        "search - semantic code search",
        "files - file/path search with fuzzy matching",
        "get_context - detailed context for results",
        "context - save/load code manifests with callees+purity",
        "list_symbols - enumerate and filter symbols (the 'ls' for code)",
        "inspect_symbol - deep inspect a single symbol",
        "browse_file - file outline view with all symbols",
        "semantic_annotations - find code by semantic tags",
        "code_insight - comprehensive codebase analysis (includes git "
        "analysis modes)",
        "side_effects - query function purity and side effects",
        "index_stats - index status and health monitoring",
        "debug_info - deep debug information for troubleshooting",
        "git_analysis - analyze git changes for quality issues",
        "info [tool] - help for specific tool (use 'info version' for "
        "server info)",
    };
    data["quick_start"] =
        "Use 'search' tool with a pattern. Use 'info search' for details.";
    data["server"] = "Lightning Code Index MCP";
    data["tagline"] = "Sub-millisecond in-memory semantic code search";
    data["why_use_lci"] = {
        "Faster than grep/rg (everything pre-indexed in memory)",
        "Smarter than find (understands code structure)",
        "Available everywhere (no IDE needed)",
        "Perfect for AI (MCP protocol, semantic output)",
    };
    return make_json_response(data);
}

// -- handle_search ------------------------------------------------------------

ToolResult handle_search(const nlohmann::json& params,
                         MasterIndex& indexer,
                         SearchEngine* search_engine) {
    // Validate required parameters
    auto validator = create_search_validator();
    auto validation = validator.validate(params);
    if (!validation.valid) {
        auto err_json = create_multi_validation_error_response(
            "search", validation.errors);
        return {err_json.dump(), true};
    }

    auto biz_err = validate_search_business_logic(params);
    if (biz_err.has_value()) {
        auto err_json = create_validation_error_response(
            "search", *biz_err);
        return {err_json.dump(), true};
    }

    auto pattern = params.value("pattern", "");
    if (pattern.empty()) {
        pattern = params.value("patterns", "");
    }
    if (pattern.empty()) {
        return make_error_response("search", "pattern is required");
    }

    // Parse options
    auto output = params.value("output", "line");
    auto flags = params.value("flags", "");
    int max_results = params.value("max", 50);
    max_results = clamp_int(max_results, 1, 100);
    int context_lines = parse_context_lines(output);

    SearchOptions options;
    options.max_results = max_results;
    options.max_context_lines = context_lines;
    options.case_insensitive = comma_list_contains(flags, "ci");
    options.word_boundary = comma_list_contains(flags, "wb");
    options.exclude_tests = comma_list_contains(flags, "nt");
    options.exclude_comments = comma_list_contains(flags, "nc");
    options.invert_match = comma_list_contains(flags, "iv");

    // Perform search
    std::vector<SearchResult> results;
    if (search_engine) {
        results = search_engine->search(pattern, options);
    } else {
        results = indexer.search_with_options(pattern, options);
    }

    // Handle files-only output
    if (output == "files") {
        std::vector<std::string> files;
        for (const auto& r : results) {
            if (files.empty() || files.back() != r.path) {
                files.push_back(r.path);
            }
        }
        nlohmann::json response;
        response["files"] = files;
        response["total_matches"] = static_cast<int>(results.size());
        response["unique_files"] = static_cast<int>(files.size());
        return make_json_response(response);
    }

    // Handle count output
    if (output == "count") {
        std::map<std::string, int> file_counts;
        for (const auto& r : results) {
            file_counts[r.path]++;
        }
        nlohmann::json response;
        response["total_matches"] = static_cast<int>(results.size());
        response["unique_files"] = static_cast<int>(file_counts.size());
        response["counts"] = file_counts;
        return make_json_response(response);
    }

    // Build standard results
    nlohmann::json result_array = nlohmann::json::array();
    for (const auto& r : results) {
        nlohmann::json item;
        item["file"] = r.path;
        item["line"] = r.line;
        item["column"] = r.column;
        item["match"] = r.match_text;
        item["score"] = static_cast<int>(r.score);

        if (!r.context.lines.empty()) {
            item["context_lines"] = r.context.lines;
        }

        result_array.push_back(std::move(item));
    }

    nlohmann::json response;
    response["results"] = std::move(result_array);
    response["total_matches"] = static_cast<int>(results.size());
    response["showing"] = static_cast<int>(results.size());
    response["max_results"] = max_results;

    return make_json_response(response);
}

// -- handle_get_context -------------------------------------------------------

ToolResult handle_get_context(const nlohmann::json& params,
                              MasterIndex& indexer) {
    auto object_id = params.value("id", "");

    auto& ref_tracker = indexer.ref_tracker();

    // -- id (object_id) lookup -- Go-shape payload --------------------------
    // Go's get_context accepts an object_id from a prior search/list call and
    // resolves it through symbol_store's deterministic indexing-order IDs.
    // Output shape (matches Go cmd/lci/mcp/handlers.go::handleGetContext):
    //   {contexts:[{file_path, line, object_id, symbol_type, symbol_name,
    //              is_exported, definition, context, purity:{...}}], count}
    // KARPATHY-RULE-2: O(1) hash lookup, no per-call allocation in the
    // resolution path itself (string conversions are unavoidable for JSON).
    // Scope-narrowed iter-9: purity block emitted with conservative defaults
    // (is_pure:false, purity_score:0, confidence:"low") because the live
    // MCP runtime never populates SideEffectAnalyzer.results() — wiring
    // analyzer into the indexing pipeline is a separate task, filed as a
    // follow-up under the loop. Inner-text canonicalize ignore covers the
    // purity sub-block until that work lands.
    if (!object_id.empty()) {
        auto decoded = decode_symbol_id(object_id);
        nlohmann::json contexts = nlohmann::json::array();
        if (decoded.has_value()) {
            const auto* sym = ref_tracker.get_enhanced_symbol(*decoded);
            if (sym != nullptr) {
                nlohmann::json ctx;
                ctx["file_path"] = indexer.get_file_path(sym->symbol.file_id);
                ctx["line"] = sym->symbol.line;
                ctx["object_id"] = object_id;
                ctx["symbol_type"] =
                    std::string(to_string(sym->symbol.type));
                ctx["symbol_name"] = std::string(sym->symbol.name);
                ctx["is_exported"] = sym->is_exported;
                ctx["definition"] = std::string(sym->symbol.name);
                ctx["context"] = nlohmann::json::array(
                    {std::string(sym->symbol.name)});
                nlohmann::json purity;
                purity["is_pure"] = false;
                purity["purity_score"] = 0;
                purity["confidence"] = "low";
                ctx["purity"] = std::move(purity);
                contexts.push_back(std::move(ctx));
            }
        }
        nlohmann::json response;
        response["contexts"] = std::move(contexts);
        response["count"] =
            static_cast<int>(response["contexts"].size());
        return make_json_response(response);
    }

    // No-mode get_context is id-only — aligned with Go iter-19.
    // Go's handleGetContext (internal/mcp/handlers.go) builds its objectIDs
    // list solely from args.ID; args.Name and file_id+line are never resolved
    // in the default (no-mode) path. C++ previously had latent name= and
    // file_id+line resolution branches here (predating iter-9, initial commit
    // 9ce02a7) that diverged from Go whenever the indexer resolved symbols
    // under the MCP stdio session. Removed to match Go's id-only contract:
    // an absent/unresolvable id yields {contexts:[],count:0}, same as Go.
    // KARPATHY parity win + perf win — this is the read path and removing the
    // branches strictly reduces work (no find_symbols_by_name / call-tree
    // traversal on the no-mode path). find_symbols_by_name / get_symbol_at_line
    // remain in use by other handlers (handlers_explore, handlers_analysis,
    // server.cpp, context_manifest_expander) — shared infra untouched.
    nlohmann::json response;
    response["contexts"] = nlohmann::json::array();
    response["count"] = 0;
    return make_json_response(response);
}

// -- handle_find_files --------------------------------------------------------

ToolResult handle_find_files(const nlohmann::json& params,
                             MasterIndex& indexer) {
    auto pattern = params.value("pattern", "");
    if (pattern.empty()) {
        return make_error_response("find_files", "pattern is required");
    }

    auto flags = params.value("flags", "");
    auto filter = params.value("filter", "");
    auto directory = params.value("directory", "");
    bool include_hidden = params.value("include_hidden", false);
    int max_results = params.value("max", 50);
    max_results = clamp_int(max_results, 1, 200);

    bool case_insensitive = comma_list_contains(flags, "ci");
    bool exact_only = comma_list_contains(flags, "exact");

    // Prepare pattern for matching
    std::string normalized_pattern = pattern;
    if (case_insensitive) {
        normalized_pattern = to_lower(pattern);
    }

    // Get all file IDs from the index
    auto snapshot = indexer.read_snapshot();
    if (!snapshot || snapshot->file_count() == 0) {
        return make_error_response("find_files", "no files in index");
    }

    struct FileMatch {
        std::string path;
        double score;
        std::string match_type;
        int file_id;
    };
    std::vector<FileMatch> matches;

    for (const auto& [path, fid] : snapshot->file_map) {
        // Directory filter
        if (!directory.empty()) {
            if (path.substr(0, directory.size()) != directory ||
                (path.size() > directory.size() &&
                 path[directory.size()] != '/')) {
                continue;
            }
        }

        // Hidden file filter
        if (!include_hidden) {
            bool hidden = false;
            size_t pos = 0;
            while (pos < path.size()) {
                auto sep = path.find('/', pos);
                auto component =
                    (sep == std::string::npos)
                        ? path.substr(pos)
                        : path.substr(pos, sep - pos);
                if (!component.empty() && component[0] == '.' &&
                    component != "." && component != "..") {
                    hidden = true;
                    break;
                }
                if (sep == std::string::npos) break;
                pos = sep + 1;
            }
            if (hidden) continue;
        }

        // File type/glob filter
        if (!filter.empty()) {
            bool is_language_filter =
                filter.find('*') == std::string::npos &&
                filter.find('.') == std::string::npos;
            if (is_language_filter) {
                auto dot_pos = path.rfind('.');
                if (dot_pos == std::string::npos) continue;
                auto ext = path.substr(dot_pos + 1);
                if (to_lower(ext) != to_lower(filter)) continue;
            } else {
                // Simple glob: match basename against pattern
                auto slash_pos = path.rfind('/');
                auto basename =
                    (slash_pos == std::string::npos)
                        ? path
                        : path.substr(slash_pos + 1);
                // Simple wildcard match: *.ext
                if (filter.size() > 2 && filter[0] == '*' &&
                    filter[1] == '.') {
                    auto ext_filter = filter.substr(1);
                    if (basename.size() < ext_filter.size() ||
                        basename.substr(basename.size() -
                                        ext_filter.size()) != ext_filter) {
                        continue;
                    }
                }
            }
        }

        // Matching
        std::string match_path = path;
        if (case_insensitive) {
            match_path = to_lower(path);
        }

        auto slash_pos = path.rfind('/');
        std::string filename =
            (slash_pos == std::string::npos) ? path
                                             : path.substr(slash_pos + 1);
        auto dot_pos = filename.rfind('.');
        std::string filename_no_ext =
            (dot_pos == std::string::npos)
                ? filename
                : filename.substr(0, dot_pos);

        std::string norm_filename = filename;
        std::string norm_filename_no_ext = filename_no_ext;
        if (case_insensitive) {
            norm_filename = to_lower(filename);
            norm_filename_no_ext = to_lower(filename_no_ext);
        }

        double score = 0.0;
        std::string match_type;

        // 1. Exact full path match
        if (match_path == normalized_pattern) {
            score = 1.0;
            match_type = "exact";
        } else if (!exact_only) {
            // 2. Exact filename match
            if (norm_filename == normalized_pattern) {
                score = 0.95;
                match_type = "exact_filename";
            } else if (norm_filename_no_ext == normalized_pattern) {
                score = 0.93;
                match_type = "exact_filename_noext";
            }

            // 3. Substring match
            if (score == 0.0) {
                auto idx = match_path.find(normalized_pattern);
                if (idx != std::string::npos) {
                    score = 0.8 - (static_cast<double>(idx) /
                                   static_cast<double>(match_path.size()) *
                                   0.2);
                    match_type = "substring";
                }
            }

            // 4. Path component match
            if (score == 0.0) {
                size_t cpos = 0;
                while (cpos < match_path.size()) {
                    auto sep = match_path.find('/', cpos);
                    auto component =
                        (sep == std::string::npos)
                            ? match_path.substr(cpos)
                            : match_path.substr(cpos, sep - cpos);
                    if (component.find(normalized_pattern) !=
                        std::string::npos) {
                        score = 0.6;
                        match_type = "path_component";
                        break;
                    }
                    if (sep == std::string::npos) break;
                    cpos = sep + 1;
                }
            }
        }

        if (score > 0.0) {
            matches.push_back(
                {path, score, match_type, static_cast<int>(fid)});
        }
    }

    // Sort by score descending
    std::sort(matches.begin(), matches.end(),
              [](const FileMatch& a, const FileMatch& b) {
                  return a.score > b.score;
              });

    // Limit results
    if (static_cast<int>(matches.size()) > max_results) {
        matches.resize(static_cast<size_t>(max_results));
    }

    // Build response
    nlohmann::json result_array = nlohmann::json::array();
    for (const auto& m : matches) {
        nlohmann::json item;
        item["path"] = m.path;
        item["score"] = m.score;
        item["match_type"] = m.match_type;
        item["file_id"] = m.file_id;
        result_array.push_back(std::move(item));
    }

    nlohmann::json response;
    response["results"] = std::move(result_array);
    response["total_matches"] = static_cast<int>(matches.size());
    response["pattern"] = pattern;

    return make_json_response(response);
}

// -- register_core_handlers ---------------------------------------------------

void register_core_handlers(McpServer& server, MasterIndex* indexer,
                            SearchEngine* search_engine) {
    // Replace "info" stub with real handler
    server.add_tool(
        {"info",
         "Get detailed help and examples for any tool. Use 'info' for "
         "overview or 'info <tool>' for specifics.",
         {{"tool", "string",
           "Tool name to get information about (e.g., 'search', "
           "'get_context', 'version')",
           ""}},
         {}},
        [](const nlohmann::json& p) { return handle_info(p); });

    // Replace "search" stub with real handler
    server.add_tool(
        {"search",
         "Sub-millisecond in-memory semantic code search. Use instead of "
         "grep, rg, find.",
         {{"pattern", "string", "Search pattern", ""},
          {"max", "integer", "Maximum results", ""},
          {"output", "string", "Output format", ""},
          {"filter", "string", "File filter", ""},
          {"flags", "string", "Search flags", ""},
          {"include", "string", "Include options", ""},
          {"symbol_types", "string",
           "Symbol types to filter (comma-separated)", ""},
          {"patterns", "string", "Multiple patterns (OR logic)", ""},
          {"max_per_file", "integer", "Max results per file", ""},
          {"semantic", "boolean", "Enable semantic search", ""},
          {"languages", "array",
           "Filter by programming languages", "string"}},
         {"pattern"}},
        [indexer, search_engine](const nlohmann::json& p) -> ToolResult {
            if (!indexer) {
                return make_error_response("search",
                                           "index not available");
            }
            return handle_search(p, *indexer, search_engine);
        });

    // Replace "get_context" stub with real handler
    server.add_tool(
        {"get_context",
         "Get detailed context for specific code objects. Use 'name' for "
         "symbol lookup or 'file_id'+'line' for location lookup.",
         {{"id", "string",
           "Concise object ID(s) from search results", ""},
          {"name", "string", "Symbol name for direct lookup", ""},
          {"file_id", "integer", "File ID to narrow lookup scope", ""},
          {"line", "integer", "Line number", ""},
          {"column", "integer", "Column number", ""},
          {"mode", "string", "Lookup mode", ""},
          {"include_full_symbol", "boolean", "Include full symbol info", ""},
          {"include_call_hierarchy", "boolean",
           "Include call hierarchy", ""},
          {"include_all_references", "boolean",
           "Include references", ""},
          {"include_dependencies", "boolean",
           "Include dependencies", ""},
          {"include_file_context", "boolean",
           "Include file context", ""},
          {"include_quality_metrics", "boolean",
           "Include quality metrics", ""},
          {"max_depth", "integer", "Max depth", ""},
          {"include_ai_text", "boolean", "Include AI text", ""},
          {"confidence_threshold", "number",
           "Confidence threshold", ""},
          {"exclude_test_files", "boolean",
           "Exclude test files", ""},
          {"include_sections", "array", "Include sections", "string"},
          {"exclude_sections", "array", "Exclude sections", "string"}},
         {}},
        [indexer](const nlohmann::json& p) -> ToolResult {
            if (!indexer) {
                return make_error_response("get_context",
                                           "index not available");
            }
            return handle_get_context(p, *indexer);
        });

    // Replace "find_files" stub with real handler
    server.add_tool(
        {"find_files",
         "Like 'find' or 'fd' - searches file paths on an in-memory "
         "index. Supports fuzzy matching and glob patterns.",
         {{"pattern", "string", "File/path pattern to search for", ""},
          {"max", "integer", "Maximum results (default: 50)", ""},
          {"filter", "string", "Filter by file type or glob", ""},
          {"flags", "string", "Search flags: 'ci', 'exact'", ""},
          {"include_hidden", "boolean",
           "Include hidden files/directories", ""},
          {"directory", "string",
           "Directory to search within", ""}},
         {"pattern"}},
        [indexer](const nlohmann::json& p) -> ToolResult {
            if (!indexer) {
                return make_error_response("find_files",
                                           "index not available");
            }
            return handle_find_files(p, *indexer);
        });
}

}  // namespace mcp
}  // namespace lci
