#include <lci/mcp/handlers_core.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json-schema.hpp>
#include <rapidfuzz/distance/Levenshtein.hpp>

#include <lci/core/reference_tracker.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/pagination.h>
#include <lci/mcp/schemas/search.h>  // generated: kSEARCH_SCHEMA
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

// -- json-schema validation glue ---------------------------------------------
//
// FIX-B (4RfLnLqNCD7u): handle_search now uses nlohmann-json-schema-validator
// against an embedded schema (share/lci/mcp-schemas/search.json) instead of
// the hand-rolled RequestValidator. Wire format is preserved by mapping the
// validator's per-error callbacks into the existing ValidationError struct
// and routing through create_(multi_)validation_error_response.
//
// karpathy: validator is built ONCE at first call (static const), not
// per-request. /search is a hot read path — no malloc/parse on each invoke.

/// Collects schema validation errors into the project's ValidationError shape.
/// Used as nlohmann::json_schema::basic_error_handler subclass.
class SearchSchemaErrorCollector
    : public nlohmann::json_schema::basic_error_handler {
  public:
    void error(const nlohmann::json::json_pointer& ptr,
               const nlohmann::json& instance,
               const std::string& message) override {
        nlohmann::json_schema::basic_error_handler::error(ptr, instance,
                                                          message);
        ValidationError err;
        err.value = instance;
        err.message = message;
        // Pointer like "/max" or "" (root). Derive a human field name.
        auto p = ptr.to_string();
        if (p.empty()) {
            err.field = "(root)";
        } else if (!p.empty() && p.front() == '/') {
            err.field = p.substr(1);  // strip leading slash
        } else {
            err.field = p;
        }
        // Best-effort code mapping from the schema validator's message text.
        // The library does not expose a structured kind, so we sniff the
        // message — matches the kRequired/kInvalidFormat/etc taxonomy the
        // existing wire format uses.
        auto lower = message;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower.find("required") != std::string::npos) {
            err.code = ValidationErrorCode::kRequired;
        } else if (lower.find("unexpected") != std::string::npos ||
                   lower.find("additional") != std::string::npos ||
                   lower.find("evaluating") != std::string::npos) {
            err.code = ValidationErrorCode::kInvalidFormat;
        } else if (lower.find("minimum") != std::string::npos ||
                   lower.find("maximum") != std::string::npos ||
                   lower.find("range") != std::string::npos) {
            err.code = ValidationErrorCode::kOutOfRange;
        } else if (lower.find("length") != std::string::npos) {
            err.code = lower.find("min") != std::string::npos
                          ? ValidationErrorCode::kTooShort
                          : ValidationErrorCode::kTooLong;
        } else {
            err.code = ValidationErrorCode::kInvalidFormat;
        }
        errors_.push_back(std::move(err));
    }

    std::vector<ValidationError> take() { return std::move(errors_); }

  private:
    std::vector<ValidationError> errors_;
};

/// Returns the lazily-initialized json-schema validator for `search` params.
/// Constructed once per process; subsequent calls are cheap pointer fetches.
const nlohmann::json_schema::json_validator& search_schema_validator() {
    static const nlohmann::json_schema::json_validator instance = [] {
        nlohmann::json_schema::json_validator v;
        v.set_root_schema(
            nlohmann::json::parse(lci::mcp::schemas::kSEARCH_SCHEMA));
        return v;
    }();
    return instance;
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
    // Validate parameters against the embedded JSON Schema. Validator is
    // built once per process (see search_schema_validator()) — karpathy:
    // no per-request allocation on the hot read path.
    SearchSchemaErrorCollector collector;
    search_schema_validator().validate(params, collector);
    auto schema_errors = collector.take();
    if (!schema_errors.empty()) {
        // Preserve wire format: single-error -> create_validation_error_response,
        // multi-error -> create_multi_validation_error_response. The existing
        // handler used the multi shape unconditionally; keep that to avoid a
        // golden churn for callers already parsing validation_errors[].
        auto err_json = create_multi_validation_error_response(
            "search", schema_errors);
        return {err_json.dump(), true};
    }

    // Business rule: schema can't express "at least one of {pattern, patterns}
    // is non-empty" as cleanly as the existing helper. Schema marks both
    // optional; this guard enforces the OR. Kept here (not in schema) so the
    // error code stays kRequired and the wire-format snapshot for missing
    // pattern is byte-identical to pre-FIX-B output.
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
    // Go's MCP search defaults to case-insensitive (verified on multi-lang
    // corpus: pattern "add" matches "Add" in a.go without an explicit flag).
    // C++ default mirrors that. Callers can still pass flags=cs to force
    // case-sensitive matching.
    options.case_insensitive = !comma_list_contains(flags, "cs");
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

    // Build standard results — Go-shape, every item is symbol-enriched.
    // Go's handleSearch (cmd/lci/mcp/handlers.go) attributes each text match
    // to its enclosing symbol so MCP callers can hand the object_id straight
    // to get_context. We mirror the shape: result_id, object_id, file, line,
    // column, match, score (float), symbol_type, symbol_name, is_exported.
    auto& ref_tracker = indexer.ref_tracker();
    nlohmann::json result_array = nlohmann::json::array();
    int ordinal = 0;
    for (const auto& r : results) {
        ++ordinal;
        nlohmann::json item;
        item["result_id"] = "result_" + std::to_string(ordinal) + "_" +
                            std::to_string(r.line);
        item["file"] = r.path;
        item["line"] = r.line;
        item["column"] = r.column;
        item["match"] = r.match_text;
        item["score"] = r.score;  // float; Go emits float, not int

        // Enclosing-symbol enrichment. ref_tracker maps (file_id, line) to
        // the EnhancedSymbol that covers the line, O(1) hash lookup per
        // call (KARPATHY rule 2 — no allocation in the inner loop).
        const auto* sym =
            ref_tracker.get_symbol_at_line(r.file_id, r.line);
        if (sym != nullptr) {
            item["object_id"] = encode_symbol_id(sym->id);
            item["symbol_name"] = std::string(sym->symbol.name);
            item["symbol_type"] =
                std::string(to_string(sym->symbol.type));
            item["is_exported"] = sym->is_exported;
        } else {
            // Match falls outside any indexed symbol (package decl, blank
            // line, comment between symbols). Emit empty enrichment fields
            // rather than omitting them so the response shape is stable.
            item["object_id"] = "";
            item["symbol_name"] = "";
            item["symbol_type"] = "";
            item["is_exported"] = false;
        }

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

// Resolves a single comma-separated object ID into `contexts`, or records a
// descriptive entry in `errors` — Go's buildObjectContextCompactWithError
// never silently drops an unresolvable id (internal/mcp/handlers.go).
void resolve_object_id(std::string_view id, MasterIndex& indexer,
                       nlohmann::json& contexts, nlohmann::json& errors) {
    auto decoded = decode_symbol_id(id);
    if (!decoded.has_value()) {
        errors.push_back({{"object_id", std::string(id)},
                          {"error", "invalid object ID: " + std::string(id)}});
        return;
    }
    const auto* sym = indexer.ref_tracker().get_enhanced_symbol(*decoded);
    if (sym == nullptr) {
        errors.push_back(
            {{"object_id", std::string(id)},
             {"error", "symbol not found: object_id=" + std::string(id) +
                           " (symbol may have been deleted or index is stale)"}});
        return;
    }

    // Go ObjectContext (internal/mcp/server.go): definition falls back to the
    // symbol name when no signature is available; signature is `omitempty`.
    std::string definition = sym->signature.empty()
                                 ? std::string(sym->symbol.name)
                                 : std::string(sym->signature);
    nlohmann::json ctx;
    ctx["file_path"] = indexer.get_file_path(sym->symbol.file_id);
    ctx["line"] = sym->symbol.line;
    ctx["object_id"] = std::string(id);
    ctx["symbol_type"] = std::string(to_string(sym->symbol.type));
    ctx["symbol_name"] = std::string(sym->symbol.name);
    ctx["is_exported"] = sym->is_exported;
    if (!sym->signature.empty()) {
        ctx["signature"] = std::string(sym->signature);
    }
    ctx["definition"] = definition;
    ctx["context"] = nlohmann::json::array({definition});
    // `purity` intentionally omitted: Go's getPurityInfo returns nil (and the
    // field is `omitempty`) whenever no side-effect propagator is wired. The
    // C++ MCP runtime exposes no side-effect analysis to this handler, so a
    // zeroed stub would both diverge from Go and violate the no-silent-stub
    // rule. Real purity wiring is a tracked follow-up.
    contexts.push_back(std::move(ctx));
}

ToolResult handle_get_context(const nlohmann::json& params,
                              MasterIndex& indexer) {
    auto object_id = params.value("id", "");
    auto name = params.value("name", "");
    auto mode = params.value("mode", "");

    // Go validateGetContextParams (internal/mcp/handlers.go): exactly one of
    // 'id' or 'name' must be supplied. Fail fast — no silent empty result.
    const bool has_id = !object_id.empty();
    const bool has_name = !name.empty();
    if (!has_id && !has_name) {
        return make_error_response(
            "get_context",
            "missing required 'id' parameter. Use the object ID (o=XX) from "
            "search results, e.g. {\"id\": \"VE\"} or {\"id\": \"VE,tG\"}");
    }
    if (has_id && has_name) {
        return make_error_response(
            "get_context",
            "parameter conflict: use either 'id' OR 'name', not both. "
            "Prefer 'id' with object IDs from search results");
    }

    // mode-based context lookup (Go's handleGetObjectContextWithMode, backed
    // by ContextLookupEngine) is not ported to C++ — there is no C++
    // equivalent of that engine. Fail fast rather than emit a silent empty
    // stub. Accepted divergence, documented in tests/parity/KNOWN_FAILURES.md.
    if (!mode.empty()) {
        return make_error_response(
            "get_context",
            "mode-based context lookup ('mode' parameter) is not supported "
            "in the C++ port; omit 'mode' and use plain 'id' lookup");
    }

    // No-mode path: Go builds its objectIDs list solely from args.ID
    // (comma-separated). args.Name passes validation but is never resolved
    // here — a name-only call yields an empty result set, matching Go's
    // id-only no-mode contract (golden: {contexts:[],count:0}).
    nlohmann::json contexts = nlohmann::json::array();
    nlohmann::json errors = nlohmann::json::array();
    std::string_view remaining = object_id;
    while (!remaining.empty()) {
        auto comma = remaining.find(',');
        std::string_view id = comma == std::string_view::npos
                                  ? remaining
                                  : remaining.substr(0, comma);
        remaining = comma == std::string_view::npos
                        ? std::string_view{}
                        : remaining.substr(comma + 1);
        // Trim surrounding whitespace (Go strings.TrimSpace per id).
        while (!id.empty() && std::isspace(static_cast<unsigned char>(id.front())))
            id.remove_prefix(1);
        while (!id.empty() && std::isspace(static_cast<unsigned char>(id.back())))
            id.remove_suffix(1);
        if (id.empty()) continue;
        resolve_object_id(id, indexer, contexts, errors);
    }

    nlohmann::json response;
    response["count"] = static_cast<int>(contexts.size());
    response["contexts"] = std::move(contexts);
    // Go reports per-id lookup failures in a `errors` array — never dropped.
    if (!errors.empty()) {
        response["errors"] = std::move(errors);
    }
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

            // 4. Fuzzy match on filenameNoExt — parity with Go's
            //    matchFilePaths step 4 (handlers_files.go:226-232).
            //    Go invokes phraseMatcher.Match(pattern, filenameNoExt) with a
            //    levenshtein FuzzyMatcher at threshold 0.7. The Go fuzzer has a
            //    quirk: edlib.StringsSimilarity returns proper similarity but
            //    levenshteinSimilarity then computes `1 - that`, so wildly
            //    different strings score ~1.0 while near-matches score low.
            //    We reproduce that observable behaviour exactly so the parity
            //    descriptor (mcp/find_files/basic) keeps yielding the same
            //    fuzzy hits with score 0.574 on the multi-lang corpus.
            //
            //    For single-word queries (no spaces) the PhraseMatcher reduces
            //    to a known closed-form: queryWords=[pattern], one fuzzy match
            //    against the (single) target word →
            //      avgWordScore = sim_norm * 0.85
            //      + exactPhraseBonus 0.05   (allWordsMatched && inOrder)
            //      − fuzzyPenalty 0.08       (fuzzyCount/matchedCount = 1)
            //      = sim_norm * 0.85 − 0.03
            //    Final find_files score = phraseScore * 0.7 (line 229).
            //    Multi-word patterns (containing whitespace) are left unscored
            //    here for now — descriptor coverage is single-word only and a
            //    full PhraseMatcher port is FIX-D.1.B-scope.
            if (score == 0.0 && !exact_only &&
                normalized_pattern.find(' ') == std::string::npos &&
                !normalized_pattern.empty() &&
                !norm_filename_no_ext.empty()) {
                // Go-compat broken levenshtein: 1.0 for total mismatch,
                // 0.0 for identical (handled by a==b early return below).
                double sim_norm;
                if (normalized_pattern == norm_filename_no_ext) {
                    sim_norm = 1.0;
                } else {
                    size_t lev = rapidfuzz::levenshtein_distance(
                        normalized_pattern, norm_filename_no_ext);
                    size_t max_len =
                        std::max(normalized_pattern.size(),
                                 norm_filename_no_ext.size());
                    // sim_norm == lev / max_len  (intentional Go bug-port)
                    sim_norm = static_cast<double>(lev) /
                               static_cast<double>(max_len);
                }
                if (sim_norm >= 0.7) {  // FuzzyMatcher threshold in Go
                    double phrase_score = sim_norm * 0.85;
                    // PhraseMatcher exactPhraseBonus (single word always
                    // counts as in-order, all-words-matched)
                    phrase_score += 0.05;
                    // PhraseMatcher fuzzyPenalty (fuzzyCount=1 of 1 match)
                    phrase_score -= 0.08;
                    if (phrase_score > 1.0) phrase_score = 1.0;
                    if (phrase_score < 0.0) phrase_score = 0.0;
                    score = phrase_score * 0.7;  // fuzzy-scale per Go
                    match_type = "fuzzy";
                }
            }

            // 5. Path component match
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
