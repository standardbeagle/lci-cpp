#include <lci/mcp/handlers_core.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <nlohmann/json-schema.hpp>
#include <rapidfuzz/distance/Levenshtein.hpp>
#include <re2/re2.h>

#include <lci/analysis/side_effect_analyzer.h>
#include <lci/core/reference_tracker.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/pagination.h>
#include <lci/mcp/schemas/search.h>  // generated: kSEARCH_SCHEMA
#include <lci/mcp/validation.h>
#include <lci/scope.h>
#include <lci/search/search_engine.h>
#include <lci/search/search_options.h>
#include <lci/version.h>  // generated: lci::kVersion

namespace lci {
namespace mcp {

// -- Helpers ------------------------------------------------------------------

namespace {

/// Parses a comma-separated string into a vector of trimmed non-empty values.
/// Go parity: parseListHelper (handlers.go:50). Pre-counts commas to size the
/// output once — no geometric realloc on the hot search path.
std::vector<std::string> parse_list_helper(std::string_view s) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    size_t expected = 1;
    for (char c : s) if (c == ',') ++expected;
    out.reserve(expected);
    size_t start = 0;
    while (start <= s.size()) {
        auto end = s.find(',', start);
        std::string_view part =
            (end == std::string_view::npos) ? s.substr(start)
                                            : s.substr(start, end - start);
        // Trim
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front())))
            part.remove_prefix(1);
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back())))
            part.remove_suffix(1);
        if (!part.empty()) out.emplace_back(part);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return out;
}

/// Infers a scope's visibility for the `breadcrumbs` add-on. Go parity:
/// inferVisibility (handlers.go ~1999): explicit Exported attribute or an
/// upper-case initial → "public"; top-level (level 0) → "public"; else
/// "private".
std::string_view infer_scope_visibility(const ScopeInfo& s) {
    for (const auto& attr : s.attributes) {
        if (attr.type == ContextAttributeType::Exported) return "public";
    }
    if (!s.name.empty() && s.name.front() >= 'A' && s.name.front() <= 'Z') {
        return "public";
    }
    return s.level == 0 ? "public" : "private";
}

/// Serializes an EnhancedSymbol's scope chain into the Go ScopeBreadcrumb
/// shape (server.go:489). One object per enclosing scope.
nlohmann::json scope_chain_to_breadcrumbs(const EnhancedSymbol& sym) {
    nlohmann::json crumbs = nlohmann::json::array();
    crumbs.get_ref<nlohmann::json::array_t&>().reserve(sym.scope_chain.size());
    for (const auto& s : sym.scope_chain) {
        crumbs.push_back({{"scope_type", std::string(to_string(s.type))},
                          {"name", s.name},
                          {"start_line", s.start_line},
                          {"end_line", s.end_line},
                          {"language", s.language},
                          {"visibility", std::string(infer_scope_visibility(s))}});
    }
    return crumbs;
}

/// Builds a path-include regex from a list of language names.
/// Go parity: languagesToIncludePattern (handlers.go:967). Returns empty
/// string when no language maps; caller treats empty as "no filter".
std::string language_array_to_file_extensions(
    const std::vector<std::string>& languages) {
    if (languages.empty()) return "";

    // Same table as Go languageToExtensions (handlers.go:926). Stored as
    // lowercase keys + alias normalization so users can pass "ts", "TypeScript",
    // "typescript" interchangeably.
    static const std::map<std::string, std::vector<std::string>> kTable = {
        {"go", {"go"}},
        {"javascript", {"js", "jsx", "mjs", "cjs"}},
        {"typescript", {"ts", "tsx", "mts", "cts"}},
        {"python", {"py", "pyw", "pyi"}},
        {"java", {"java"}},
        {"rust", {"rs"}},
        {"c++", {"cpp", "cc", "cxx", "hpp", "hxx", "h++"}},
        {"cpp", {"cpp", "cc", "cxx", "hpp", "hxx", "h++"}},
        {"c", {"c", "h"}},
        {"c#", {"cs"}},
        {"csharp", {"cs"}},
        {"php", {"php", "phtml"}},
        {"ruby", {"rb", "rake", "gemspec"}},
        {"swift", {"swift"}},
        {"kotlin", {"kt", "kts"}},
        {"scala", {"scala", "sc"}},
        {"vue", {"vue"}},
        {"svelte", {"svelte"}},
        {"dart", {"dart"}},
        {"zig", {"zig"}},
        {"shell", {"sh", "bash", "zsh"}},
        {"html", {"html", "htm"}},
        {"css", {"css", "scss", "sass", "less"}},
        {"sql", {"sql"}},
        {"markdown", {"md", "markdown"}},
        {"json", {"json"}},
        {"yaml", {"yaml", "yml"}},
        {"xml", {"xml"}},
        {"lua", {"lua"}},
        {"r", {"r"}},
        {"perl", {"pl", "pm"}},
        {"haskell", {"hs", "lhs"}},
        {"elixir", {"ex", "exs"}},
        {"erlang", {"erl", "hrl"}},
        {"clojure", {"clj", "cljs", "cljc"}},
        {"ocaml", {"ml", "mli"}},
        {"f#", {"fs", "fsi", "fsx"}},
        // Common aliases for short forms.
        {"ts", {"ts", "tsx", "mts", "cts"}},
        {"js", {"js", "jsx", "mjs", "cjs"}},
        {"py", {"py", "pyw", "pyi"}},
        {"rb", {"rb", "rake", "gemspec"}},
        {"cs", {"cs"}},
        {"kt", {"kt", "kts"}},
    };

    std::vector<std::string> exts;
    exts.reserve(languages.size() * 2);
    for (const auto& lang : languages) {
        std::string lower;
        lower.reserve(lang.size());
        for (char c : lang) {
            lower.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        }
        auto it = kTable.find(lower);
        if (it == kTable.end()) continue;
        for (const auto& e : it->second) {
            // Dedup.
            bool seen = false;
            for (const auto& cur : exts) if (cur == e) { seen = true; break; }
            if (!seen) exts.push_back(e);
        }
    }
    if (exts.empty()) return "";

    std::string out;
    out.reserve(exts.size() * 5 + 8);
    out.append("\\.(");
    for (size_t i = 0; i < exts.size(); ++i) {
        if (i) out.push_back('|');
        out.append(exts[i]);
    }
    out.append(")$");
    return out;
}

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

// -- get_context helpers -----------------------------------------------------
//
// Go's get_context handler does parameter normalization up-front
// (handlers.go:2074 NormalizeContextParams + extractObjectIDFromCodeInsight).
// We replicate the cheap, non-engine surfaces here: alias remap, oid=
// extraction, and a workflow-error response for the symbol+path "auto search"
// pattern (we don't have AutoSearch yet — fail-fast with a clear hint).

/// Aliases user-typed parameter names to the canonical `id`. Mutates `params`
/// in place. Returns true if any alias was rewritten.
/// Go parity: NormalizeContextParams.
bool normalize_context_params(nlohmann::json& params) {
    if (!params.is_object()) return false;
    bool rewrote = false;
    static const std::vector<std::string> kIdAliases = {
        "symbol_id", "object_id", "object_ids", "oid"};
    if (!params.contains("id")) {
        for (const auto& a : kIdAliases) {
            if (params.contains(a)) {
                params["id"] = params[a];
                params.erase(a);
                rewrote = true;
                break;
            }
        }
    } else {
        // Strip aliases even if `id` is set — Go drops them silently.
        for (const auto& a : kIdAliases) params.erase(a);
    }
    return rewrote;
}

/// Extracts trailing comma-separated object IDs from strings like
/// "oid=ABC,XY,DE". Go parity: extractObjectIDFromCodeInsight via regex
/// `oid=([A-Za-z0-9,]+)`. RE2 here, not std::regex.
std::vector<std::string> extract_oid_prefix(std::string_view s) {
    std::vector<std::string> out;
    static const RE2 kOidRe(R"(oid=([A-Za-z0-9,]+))");
    re2::StringPiece input(s.data(), s.size());
    re2::StringPiece captured;
    while (RE2::FindAndConsume(&input, kOidRe, &captured)) {
        std::string_view csv(captured.data(), captured.size());
        size_t start = 0;
        while (start <= csv.size()) {
            auto end = csv.find(',', start);
            std::string_view tok = (end == std::string_view::npos)
                                       ? csv.substr(start)
                                       : csv.substr(start, end - start);
            if (!tok.empty()) out.emplace_back(tok);
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
    }
    return out;
}

/// Mode presets (Go applyContextLookupMode, handlers.go:2327). Mutates params
/// in place. Unknown modes pass through unchanged.
void apply_context_lookup_mode(nlohmann::json& params) {
    if (!params.is_object()) return;
    std::string mode = params.value("mode", "");
    if (mode.empty()) return;  // No mode → no preset.
    auto set_if_unset = [&](const char* key, auto val) {
        if (!params.contains(key)) params[key] = val;
    };
    if (mode == "full") {
        if (!params.contains("max_depth") ||
            params["max_depth"].get<int>() == 0) {
            params["max_depth"] = 5;
        }
        set_if_unset("include_ai_text", true);
    } else if (mode == "quick") {
        params["max_depth"] = 2;
        params["include_ai_text"] = false;
        if (!params.contains("include_sections")) {
            params["include_sections"] = {"relationships", "structure"};
        }
    } else if (mode == "relationships") {
        if (!params.contains("include_sections")) {
            params["include_sections"] = {"relationships"};
        }
    } else if (mode == "semantic") {
        if (!params.contains("include_sections")) {
            params["include_sections"] = {"semantic", "ai"};
        }
    } else if (mode == "usage") {
        if (!params.contains("include_sections")) {
            params["include_sections"] = {"usage"};
        }
    } else if (mode == "variables") {
        if (!params.contains("include_sections")) {
            params["include_sections"] = {"variables"};
        }
    }
}

/// Returns true if `section` is allowed by include/exclude_sections (if set).
bool section_allowed(const nlohmann::json& params, const std::string& section) {
    if (params.contains("include_sections") &&
        params["include_sections"].is_array() &&
        !params["include_sections"].empty()) {
        bool found = false;
        for (const auto& v : params["include_sections"]) {
            if (v.is_string() && v.get<std::string>() == section) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    if (params.contains("exclude_sections") &&
        params["exclude_sections"].is_array()) {
        for (const auto& v : params["exclude_sections"]) {
            if (v.is_string() && v.get<std::string>() == section) return false;
        }
    }
    return true;
}

/// Returns the response shape for the auto-search workflow (symbol + path
/// provided instead of id). We do not implement auto-search in C++ yet;
/// return a clear workflow hint (Karpathy #6 — fail-fast, not empty).
// Go parity: autoSearchAndReturnContext (handlers.go ~2038). When a caller
// passes symbol+path but no id, return a positive workflow-hint payload that
// guides them through search -> object_id -> get_context. Not an error: the
// shape matches Go's map exactly so MCP clients can branch on
// `_auto_search_triggered`.
nlohmann::json autosearch_workflow_hint(const std::string& symbol,
                                        const std::string& path) {
    nlohmann::json data;
    data["_auto_search_triggered"] = true;
    data["symbol"] = symbol;
    data["path"] = path;
    data["message"] =
        "Auto-search is now supported! Use the search tool first, then "
        "get_context with the object_id.";
    data["workflow"] = {
        "1. Search: search {\"pattern\": \"" + symbol + "\"}",
        "2. Find object_id (o=XX) in search results",
        "3. Get context: get_context {\"id\": \"XX\"}",
    };
    data["example_search"] =
        "{\"pattern\": \"" + symbol + "\", \"max\": 5}";
    data["hint"] =
        "The search tool will return results with object_id (o=XX) that you "
        "can use with get_context";
    return data;
}

}  // namespace

namespace {
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
    auto patterns_csv = params.value("patterns", "");
    if (pattern.empty()) {
        pattern = patterns_csv;
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
    options.use_regex = comma_list_contains(flags, "rx");

    // Semantic expansion default true to mirror MCP-side Go default
    // (cmd/lci/mcp.SearchParams.Semantic = true by handler convention).
    options.semantic = params.value("semantic", true);

    // symbol_types CSV → vector<string>. Go: parseListHelper(args.SymbolTypes).
    if (params.contains("symbol_types") && params["symbol_types"].is_string()) {
        options.symbol_types = parse_list_helper(
            params["symbol_types"].get_ref<const std::string&>());
    }

    // patterns CSV → vector<string>. Multi-pattern OR search.
    if (!patterns_csv.empty()) {
        options.pattern_list = parse_list_helper(patterns_csv);
    }

    // languages[] → include_pattern (regex matching file extensions).
    if (params.contains("languages") && params["languages"].is_array()) {
        std::vector<std::string> langs;
        langs.reserve(params["languages"].size());
        for (const auto& v : params["languages"]) {
            if (v.is_string()) langs.push_back(v.get<std::string>());
        }
        options.include_pattern =
            language_array_to_file_extensions(langs);
    }

    // filter → exclude_pattern (Go SearchOptions.ExcludePattern = args.Filter).
    if (params.contains("filter") && params["filter"].is_string()) {
        options.exclude_pattern = params["filter"].get<std::string>();
    }

    int max_per_file = params.value("max_per_file", 0);
    if (max_per_file > 0) options.max_count_per_file = max_per_file;

    // `include` add-ons. Go's handleSearch (handlers.go ~1598-1607) enriches
    // each strong match (normalizedScore >= 0.5) with optional sections:
    //   refs        -> references {incoming_count, outgoing_count}
    //   breadcrumbs -> the enclosing scope chain
    //   safety/deps -> accepted but never populated in compact results
    //                  (server.go CompactSearchResult leaves them empty)
    // object_ids/ids are always emitted. Genuinely-unknown tokens fail-fast
    // (Karpathy #6; stricter than Go's silent shouldInclude=false).
    bool include_breadcrumbs = false;
    bool include_refs = false;
    if (params.contains("include") && params["include"].is_string()) {
        const auto& inc = params["include"].get_ref<const std::string&>();
        for (auto& tok : parse_list_helper(inc)) {
            if (tok.empty() || tok == "object_ids" || tok == "ids") continue;
            if (tok == "breadcrumbs") { include_breadcrumbs = true; continue; }
            if (tok == "refs") { include_refs = true; continue; }
            if (tok == "safety" || tok == "deps") continue;  // accepted, unfilled
            return make_error_response(
                "search",
                "include='" + tok + "' is not a recognized search add-on. "
                "Allowed: object_ids, breadcrumbs, refs, safety, deps.");
        }
    }

    // Perform search
    std::vector<SearchResult> results;
    auto run = [&](const std::string& p, const SearchOptions& o) {
        if (search_engine) return search_engine->search(p, o);
        return indexer.search_with_options(p, o);
    };
    auto run_multi = [&](const std::vector<std::string>& ps,
                         const std::vector<bool>& ci_flags,
                         const SearchOptions& o) {
        if (search_engine) return search_engine->search(ps, ci_flags, o);
        // Fallback for older indexers: OR-merge manually, honoring the
        // per-pattern case-insensitive override for synonym-injected patterns.
        std::vector<SearchResult> agg;
        for (size_t i = 0; i < ps.size(); ++i) {
            SearchOptions po = o;
            if (i < ci_flags.size() && ci_flags[i]) po.case_insensitive = true;
            auto rs = indexer.search_with_options(ps[i], po);
            agg.insert(agg.end(),
                       std::make_move_iterator(rs.begin()),
                       std::make_move_iterator(rs.end()));
        }
        return agg;
    };

    // Build effective pattern list. Semantic expansion fans out multi-word
    // patterns; explicit `patterns` CSV is OR-merged as-is.
    if (!options.pattern_list.empty()) {
        results = run_multi(options.pattern_list, {}, options);
    } else if (options.semantic) {
        // Synonym-aware expansion: query terms fan out to their equivalence
        // groups (login<->signin, delete/remove/erase). Synonym-injected
        // patterns are flagged so they match case-insensitively.
        const SynonymTable& syn = search_engine ? search_engine->synonyms()
                                                 : default_synonym_table();
        std::vector<bool> syn_flags;
        auto expanded = expand_pattern_semantic(pattern, syn, syn_flags);
        if (expanded.size() > 1) {
            results = run_multi(expanded, syn_flags, options);
        } else {
            results = run(pattern, options);
        }
    } else {
        results = run(pattern, options);
    }

    // Regex fallback: if pattern looks regex-y but `rx` flag was not set,
    // re-search in regex mode with reduced score and merge.
    // Mirrors Go handlers.go:1910 looksLikeRegex fallback.
    if (!options.use_regex && results.size() < static_cast<size_t>(max_results) &&
        looks_like_regex(pattern)) {
        SearchOptions rx_opts = options;
        rx_opts.use_regex = true;
        auto rx_results = run(pattern, rx_opts);
        for (auto& r : rx_results) r.score *= 0.7;
        // Merge with existing literal results, then re-rank.
        results.reserve(results.size() + rx_results.size());
        for (auto& r : rx_results) results.emplace_back(std::move(r));
        SearchCoordinator::rank(results);
        if (static_cast<int>(results.size()) > max_results) {
            results.resize(static_cast<size_t>(max_results));
        }
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
    // Pre-size to skip geometric realloc on the inner push_back loop.
    // result_array is array_t (std::vector<json>) under the hood.
    result_array.get_ref<nlohmann::json::array_t&>().reserve(results.size());
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

            // Optional include= add-ons, gated like Go on strong matches:
            // normalizedScore >= 0.5 (scores >1 are /100-normalized).
            double normalized = r.score > 1.0 ? r.score / 100.0 : r.score;
            if (normalized >= 0.5) {
                if (include_refs) {
                    item["references"] = {
                        {"incoming_count",
                         static_cast<int>(sym->incoming_refs.size())},
                        {"outgoing_count",
                         static_cast<int>(sym->outgoing_refs.size())}};
                }
                if (include_breadcrumbs && !sym->scope_chain.empty()) {
                    item["breadcrumbs"] = scope_chain_to_breadcrumbs(*sym);
                }
            }
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

// Builds the Go PurityInfo JSON block (server.go:386) for a function/method
// from its SideEffectInfo. Effect lists and reasons are omitted when empty
// (omitempty parity).
nlohmann::json purity_to_json(const SideEffectInfo& info) {
    nlohmann::json purity;
    purity["is_pure"] = info.is_pure;
    purity["purity_score"] = info.purity_score;
    purity["confidence"] = std::string(to_string(info.confidence));
    auto local = categories_to_strings(info.categories);
    if (!local.empty()) purity["local_effects"] = std::move(local);
    auto transitive = categories_to_strings(info.transitive_categories);
    if (!transitive.empty()) purity["transitive_effects"] = std::move(transitive);
    if (!info.impurity_reasons.empty()) {
        purity["reasons"] = info.impurity_reasons;
    }
    return purity;
}

// Attaches a `purity` block to a function/method context when an analyzer is
// wired. Go's getPurityInfo (handlers.go:2433) only runs for Function/Method
// and returns nil (field omitted) when no propagator/report exists.
void attach_purity(nlohmann::json& ctx, const EnhancedSymbol& sym,
                   MasterIndex& indexer, const SideEffectAnalyzer* analyzer) {
    if (analyzer == nullptr) return;
    if (sym.symbol.type != SymbolType::Function &&
        sym.symbol.type != SymbolType::Method) {
        return;
    }
    const SideEffectInfo* info = analyzer->get_result(
        indexer.get_file_path(sym.symbol.file_id), sym.symbol.line);
    if (info == nullptr) return;
    ctx["purity"] = purity_to_json(*info);
}

// Resolves a single comma-separated object ID into `contexts`, or records a
// descriptive entry in `errors` — Go's buildObjectContextCompactWithError
// never silently drops an unresolvable id (internal/mcp/handlers.go).
void resolve_object_id(std::string_view id, MasterIndex& indexer,
                       nlohmann::json& contexts, nlohmann::json& errors,
                       const SideEffectAnalyzer* analyzer) {
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
    // Go getPurityInfo: function/method symbols carry a purity block when a
    // side-effect analyzer is wired; otherwise the field is omitted.
    attach_purity(ctx, *sym, indexer, analyzer);
    contexts.push_back(std::move(ctx));
}

ToolResult handle_get_context(const nlohmann::json& params,
                              MasterIndex& indexer,
                              const SideEffectAnalyzer* analyzer) {
    // Step 0: Alias normalization. `symbol_id`, `object_id`, `object_ids`,
    // `oid` all map to `id`. Go parity (handlers.go:2075 NormalizeContextParams).
    nlohmann::json p = params;
    normalize_context_params(p);

    auto object_id = p.value("id", "");
    auto name = p.value("name", "");
    auto symbol_param = p.value("symbol", "");
    auto path_param = p.value("path", "");
    auto mode = p.value("mode", "");

    // Auto-search shape (symbol + path, no id) — fail-fast with a clear
    // hint. Karpathy #6: no silent empty stub. Tracked as loop-fix.
    if (object_id.empty() && !symbol_param.empty() && !path_param.empty()) {
        return make_json_response(
            autosearch_workflow_hint(symbol_param, path_param));
    }

    // oid= extraction (Go extractObjectIDFromCodeInsight). Lets callers paste
    // code_insight output strings like "see oid=VE,tG" straight in.
    if (!object_id.empty() &&
        object_id.find("oid=") != std::string::npos) {
        auto extracted = extract_oid_prefix(object_id);
        if (!extracted.empty()) {
            std::string joined;
            for (size_t i = 0; i < extracted.size(); ++i) {
                if (i) joined.push_back(',');
                joined.append(extracted[i]);
            }
            object_id = std::move(joined);
            p["id"] = object_id;
        }
    }

    // Apply mode-specific defaults (depth, sections, etc.) before reading
    // params downstream. mode="full" sets max_depth=5 when unset.
    apply_context_lookup_mode(p);

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

    // Section filtering: reject any include_section we cannot honor. We
    // do not have a ContextLookupEngine in C++ yet, so structure/variables/
    // semantic/usage/ai/dependencies/file_context/quality_metrics return a
    // clear "not implemented" error rather than a silent empty section.
    // The sections we DO honor: relationships, basic (the default body).
    if (p.contains("include_sections") && p["include_sections"].is_array()) {
        static const std::vector<std::string> kSupported = {
            "relationships", "basic", "callers", "callees"};
        static const std::vector<std::string> kUnsupported = {
            "structure", "variables", "semantic", "usage", "ai",
            "dependencies", "file_context", "quality_metrics"};
        for (const auto& v : p["include_sections"]) {
            if (!v.is_string()) continue;
            const auto s = v.get<std::string>();
            for (const auto& u : kUnsupported) {
                if (s == u) {
                    return make_error_response(
                        "get_context",
                        "section '" + s +
                            "' is not implemented in the C++ port (no "
                            "ContextLookupEngine yet — tracked as "
                            "loop-fix:mcp.get_context.section." + s + "). "
                            "Supported sections: relationships, basic, "
                            "callers, callees.");
                }
            }
            (void)kSupported;  // silence unused warning under -Wno-unused
        }
    }

    // mode + name path: minimal port of Go's handleGetObjectContextWithMode.
    // Full ContextLookupEngine (6261 LOC across 8 files in internal/core/
    // context_lookup_*.go) covers structure / semantic / variables /
    // usage / ai sections. We implement the subset MCP callers actually
    // exercise on the standard chi/fastapi/pocketbase tests: name →
    // EnhancedSymbol resolution + optional call hierarchy. Other sections
    // remain unported and absent from the response (omitempty-style).
    if (!mode.empty() && has_name) {
        bool include_call_hierarchy =
            p.value("include_call_hierarchy", false);
        int max_depth = p.value("max_depth", 1);
        if (max_depth < 1) max_depth = 1;
        if (max_depth > 10) max_depth = 10;
        const bool want_relationships =
            section_allowed(p, "relationships") || section_allowed(p, "callers");

        nlohmann::json contexts = nlohmann::json::array();
        auto& tracker = indexer.ref_tracker();
        auto matches = tracker.find_symbols_by_name(name);
        contexts.get_ref<nlohmann::json::array_t&>().reserve(matches.size());
        for (const auto* sym : matches) {
            if (sym == nullptr) continue;

            std::string definition = sym->signature.empty()
                                         ? std::string(sym->symbol.name)
                                         : std::string(sym->signature);
            nlohmann::json ctx;
            ctx["file_path"] = indexer.get_file_path(sym->symbol.file_id);
            ctx["line"] = sym->symbol.line;
            ctx["object_id"] = encode_symbol_id(sym->id);
            ctx["symbol_type"] = std::string(to_string(sym->symbol.type));
            ctx["symbol_name"] = std::string(sym->symbol.name);
            ctx["is_exported"] = sym->is_exported;
            if (!sym->signature.empty()) {
                ctx["signature"] = std::string(sym->signature);
            }
            ctx["definition"] = definition;
            ctx["context"] = nlohmann::json::array({definition});
            attach_purity(ctx, *sym, indexer, analyzer);

            if (include_call_hierarchy && want_relationships) {
                nlohmann::json callers = nlohmann::json::array();
                nlohmann::json callees = nlohmann::json::array();
                for (const auto& cn : tracker.get_caller_names(sym->id)) {
                    callers.push_back(cn);
                }
                for (const auto& cn : tracker.get_callee_names(sym->id)) {
                    callees.push_back(cn);
                }
                ctx["callers"] = std::move(callers);
                ctx["callees"] = std::move(callees);

                // Recursive call tree to `max_depth` levels.
                // Cycle detection via visited set keyed on SymbolID. We chase
                // callees by NAME (ReferenceTracker exposes name-based call
                // edges, not edge-IDs) — multiple symbols of the same name
                // expand under each name node, mirroring Go's BuildCallGraph
                // ambiguity when symbol names collide.
                std::function<nlohmann::json(const EnhancedSymbol*, int,
                                             absl::flat_hash_set<uint64_t>&)>
                    build_tree;
                build_tree = [&](const EnhancedSymbol* node, int depth,
                                 absl::flat_hash_set<uint64_t>& visited)
                    -> nlohmann::json {
                    nlohmann::json t;
                    if (node == nullptr) return t;
                    t["root"] = std::string(node->symbol.name);
                    nlohmann::json kids = nlohmann::json::array();
                    if (depth <= 0) {
                        t["children"] = std::move(kids);
                        return t;
                    }
                    for (const auto& cn : tracker.get_callee_names(node->id)) {
                        // Each callee NAME may resolve to multiple symbols;
                        // we only recurse into the first to bound fan-out, but
                        // emit a leaf entry for the name regardless.
                        auto sub_matches = tracker.find_symbols_by_name(cn);
                        if (sub_matches.empty()) {
                            kids.push_back({{"root", cn},
                                            {"children",
                                             nlohmann::json::array()}});
                            continue;
                        }
                        const EnhancedSymbol* child = sub_matches.front();
                        if (child == nullptr) continue;
                        uint64_t key =
                            static_cast<uint64_t>(child->id);
                        if (!visited.insert(key).second) {
                            kids.push_back({{"root", cn},
                                            {"children",
                                             nlohmann::json::array()},
                                            {"cycle", true}});
                            continue;
                        }
                        kids.push_back(
                            build_tree(child, depth - 1, visited));
                    }
                    t["children"] = std::move(kids);
                    return t;
                };
                absl::flat_hash_set<uint64_t> visited;
                visited.insert(static_cast<uint64_t>(sym->id));
                ctx["call_tree"] = build_tree(sym, max_depth - 1, visited);
            }

            contexts.push_back(std::move(ctx));
        }

        nlohmann::json response;
        response["count"] = static_cast<int>(contexts.size());
        response["contexts"] = std::move(contexts);
        return make_json_response(response);
    }

    // No-mode path: Go builds its objectIDs list solely from args.ID
    // (comma-separated). args.Name passes Go's validateGetContextParams
    // but is never resolved here — Go's id-only no-mode contract yields
    // {contexts:[],count:0} for name-only invocations. Mirror that
    // exactly per Karpathy rule 1 (Go is the bar).
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
        resolve_object_id(id, indexer, contexts, errors, analyzer);
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
        int pattern_count{1};
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
                {path, score, match_type, static_cast<int>(fid), 1});
        }
    }

    // Multi-word coverage: when the user types "user controller handler" we
    // re-scan with each word separately (>2 chars) and boost files matching
    // more words. Go parity: matchFilePaths multi-word boost. Karpathy: build
    // a path→index map once, no allocs in the per-word inner loop.
    if (pattern.find(' ') != std::string::npos) {
        std::vector<std::string> words;
        split_on_spaces(pattern, words);
        // Filter to >2-char words, deduped, excluding the full pattern itself.
        std::vector<std::string> extra;
        extra.reserve(words.size());
        for (auto& w : words) {
            if (w.size() <= 2) continue;
            if (w == pattern) continue;
            bool seen = false;
            for (const auto& e : extra) if (e == w) { seen = true; break; }
            if (!seen) extra.push_back(std::move(w));
        }
        if (extra.size() >= 1) {
            absl::flat_hash_map<std::string, size_t> by_path;
            by_path.reserve(matches.size());
            for (size_t i = 0; i < matches.size(); ++i) {
                by_path.emplace(matches[i].path, i);
            }
            for (const auto& w : extra) {
                std::string norm_w = case_insensitive ? to_lower(w) : w;
                for (const auto& [path, fid] : snapshot->file_map) {
                    // Same hidden + directory + filter rules as above. We
                    // skip the heavyweight per-pattern fuzzy/exact branches
                    // and use a single substring-match heuristic for the
                    // word pass — exact / substring / path-component only.
                    if (!directory.empty()) {
                        if (path.substr(0, directory.size()) != directory ||
                            (path.size() > directory.size() &&
                             path[directory.size()] != '/')) continue;
                    }
                    std::string mp = case_insensitive ? to_lower(path) : path;
                    if (mp.find(norm_w) == std::string::npos) continue;
                    auto it = by_path.find(path);
                    if (it != by_path.end()) {
                        // File already matched main pattern: increment count.
                        ++matches[it->second].pattern_count;
                    } else {
                        matches.push_back(
                            {path, 0.5, "word_substring",
                             static_cast<int>(fid), 1});
                        by_path.emplace(matches.back().path,
                                        matches.size() - 1);
                    }
                }
            }
            // Apply coverage boost: +0.15 per additional pattern match, cap
            // +0.5; final score clamped to ≤1.0 per spec.
            for (auto& m : matches) {
                if (m.pattern_count > 1) {
                    double extra_boost =
                        static_cast<double>(m.pattern_count - 1) * 0.15;
                    if (extra_boost > 0.5) extra_boost = 0.5;
                    m.score += extra_boost;
                    if (m.score > 1.0) m.score = 1.0;
                }
            }
        }
    }

    // Sort by score descending, with deterministic tiebreakers.
    // file_map is absl::flat_hash_map → iteration order is non-deterministic.
    // Without explicit tiebreak, equal-score matches surface in hash order.
    // Tiebreak: file_id ascending (matches Go golden ordering), then path.
    std::sort(matches.begin(), matches.end(),
              [](const FileMatch& a, const FileMatch& b) {
                  if (a.score != b.score) return a.score > b.score;
                  if (a.file_id != b.file_id) return a.file_id < b.file_id;
                  return a.path < b.path;
              });

    // Limit results
    if (static_cast<int>(matches.size()) > max_results) {
        matches.resize(static_cast<size_t>(max_results));
    }

    // Build response
    nlohmann::json result_array = nlohmann::json::array();
    result_array.get_ref<nlohmann::json::array_t&>().reserve(matches.size());
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
                            SearchEngine* search_engine,
                            SideEffectAnalyzer* analyzer) {
    // Replace "info" stub with real handler
    server.add_tool(
        {"info",
         "🔍 Get detailed help and examples for any tool - start here! Use "
         "'info' for overview or 'info <tool>' for specifics. Use 'info "
         "version' for server version info.",
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
         "grep, rg, find. Note: Uses JSON parameters, not CLI flags like "
         "-n. See 'info search' for parameter details.",
         {{"pattern", "string", "Search pattern", ""},
          {"max", "integer", "Maximum results", ""},
          {"output", "string", "Output format", ""},
          {"filter", "string", "File filter", ""},
          {"flags", "string", "Search flags", ""},
          {"include", "string", "Include options", ""},
          {"symbol_types", "string",
           "Symbol types to filter results (comma-separated). Valid types: "
           "function, class, method, variable, constant, interface, type, "
           "struct, module, namespace, property, event, delegate, enum, "
           "record, operator, indexer, object, companion, extension, "
           "annotation, field, enum_member. Aliases: func->function, "
           "var->variable, const->constant, cls->class, meth->method, "
           "iface->interface, def->function (Python), fn->function (Rust), "
           "trait->interface (Rust). Prefix and fuzzy matching supported "
           "with warnings.",
           ""},
          {"patterns", "string", "Multiple patterns", ""},
          {"max_per_file", "integer", "Max per file", ""},
          {"semantic", "boolean", "Enable semantic", ""},
          {"languages", "array",
           "Filter by programming languages (e.g., [\"go\"], "
           "[\"typescript\", \"javascript\"], [\"csharp\"]). "
           "Case-insensitive with aliases (e.g., 'ts' for TypeScript, 'cs' "
           "for C#).",
           "string"}},
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
         "📋 Get detailed context for specific code objects. Use the 'id' "
         "parameter with object IDs from search results. See 'info "
         "get_context' for examples.",
         {{"id", "string",
           "Concise object ID(s) from search results (e.g., \"VE\" or "
           "\"VE,tG\" for multiple)",
           ""},
          {"name", "string", "Symbol name for direct lookup (alternative "
                             "to id)",
           ""},
          {"file_id", "integer", "File ID to narrow name lookup scope", ""},
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
        [indexer, analyzer](const nlohmann::json& p) -> ToolResult {
            if (!indexer) {
                return make_error_response("get_context",
                                           "index not available");
            }
            return handle_get_context(p, *indexer, analyzer);
        });

    // Replace "find_files" stub with real handler
    server.add_tool(
        {"find_files",
         "📁 Like 'find' or 'fd' - searches file paths, not content, on an "
         "in-memory index. Supports fuzzy matching, glob patterns, and "
         "filters. See 'info find_files'.",
         {{"pattern", "string",
           "File/path pattern to search for (supports fuzzy matching)", ""},
          {"max", "integer", "Maximum results (default: 50, max: 200)", ""},
          {"filter", "string",
           "Filter by file type or glob pattern (e.g., 'go', '*.ts', "
           "'src/**/*.js')",
           ""},
          {"flags", "string",
           "Search flags: 'ci' (case-insensitive), 'exact' (exact match "
           "only)",
           ""},
          {"include_hidden", "boolean",
           "Include hidden files/directories (default: false)", ""},
          {"directory", "string",
           "Directory to search within (relative to project root)", ""}},
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
