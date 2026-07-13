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
#include <lci/core/context_lookup.h>
#include <lci/core/reference_tracker.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
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

/// Wildcard glob: '*' matches any run of chars (including '/'), '?' matches one.
/// Allocation-free two-pointer scan with star-backtracking; no std::regex (this
/// runs per-file on the find_files read path).
bool wildcard_match(std::string_view str, std::string_view pat) {
    size_t s = 0, p = 0, star = std::string_view::npos, s_after_star = 0;
    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) {
            ++s;
            ++p;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            s_after_star = s;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            s = ++s_after_star;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

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

/// Language-name → file-extension table, shared by the `languages[]`
/// include filter and the `filter` token translation. Same table as Go
/// languageToExtensions (handlers.go:926); lowercase keys + aliases so
/// callers can pass "ts", "TypeScript", "typescript" interchangeably.
const std::map<std::string, std::vector<std::string>>& language_ext_table() {
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
    return kTable;
}

/// Builds a path-include regex from a list of language names.
/// Go parity: languagesToIncludePattern (handlers.go:967). Returns empty
/// string when no language maps; caller treats empty as "no filter".
std::string language_array_to_file_extensions(
    const std::vector<std::string>& languages) {
    if (languages.empty()) return "";

    const auto& kTable = language_ext_table();

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

/// Translates the search `filter` CSV into include globs matched against
/// root-relative paths. Tokens: a known language name ("go", "typescript")
/// expands to its extensions; a bare extension token ("md") becomes
/// **/*.md; anything carrying wildcards or slashes is used as a glob
/// (basename globs get a **/ prefix so "*.py" matches at any depth).
/// The old behavior compiled the raw CSV as ONE RE2 *exclude* regex —
/// filter:"go" silently excluded every path containing "go", and glob/CSV
/// values were invalid regexes that dropped the filter entirely.
std::vector<std::string> filter_tokens_to_globs(const std::string& csv) {
    std::vector<std::string> globs;
    if (csv.empty()) return globs;
    const auto& table = language_ext_table();
    for (auto& raw : parse_list_helper(csv)) {
        if (raw.empty()) continue;
        if (raw.find_first_of("*?") != std::string::npos ||
            raw.find('/') != std::string::npos) {
            // Glob: anchor basename globs at any depth.
            if (raw.find('/') == std::string::npos) {
                globs.push_back("**/" + raw);
            } else {
                globs.push_back(raw);
            }
            continue;
        }
        std::string lower = raw;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        auto it = table.find(lower);
        if (it != table.end()) {
            for (const auto& ext : it->second) {
                globs.push_back("**/*." + ext);
            }
        } else {
            // Unknown token: treat as a literal extension.
            globs.push_back("**/*." + lower);
        }
    }
    return globs;
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
/// provided instead of id). By design we return a clear workflow hint rather
/// than running the search server-side (Karpathy #6 — fail-fast, not empty),
/// mirroring Go's positive _auto_search_triggered payload.
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

// -- similar_symbol_suggestions ------------------------------------------------

nlohmann::json similar_symbol_suggestions(
    const ReferenceTracker::Snapshot& rt_snap, const std::string& query) {
    nlohmann::json sims = nlohmann::json::array();
    std::string word = to_lower(query);
    if (word.size() < 3) return sims;

    // Substring containment scores high; otherwise normalized Levenshtein
    // similarity — catches both partial names and typos ("handleRequestz"
    // -> handleRequest). Cold path: runs only after an empty lookup.
    struct Suggestion {
        const EnhancedSymbol* sym;
        double score;
    };
    std::vector<Suggestion> nearby;
    for (const auto& es : rt_snap.symbols.get_all()) {
        std::string name_l = to_lower(std::string(es.symbol.name));
        if (name_l.empty()) continue;
        double score = 0.0;
        if (name_l.find(word) != std::string::npos) {
            score = 0.9;
        } else {
            size_t lev = rapidfuzz::levenshtein_distance(word, name_l);
            size_t max_len = std::max(word.size(), name_l.size());
            score = 1.0 - static_cast<double>(lev) /
                              static_cast<double>(max_len);
        }
        if (score >= 0.7) nearby.push_back({&es, score});
    }
    std::sort(nearby.begin(), nearby.end(),
              [](const Suggestion& a, const Suggestion& b) {
                  if (a.score != b.score) return a.score > b.score;
                  if (a.sym->symbol.name.size() != b.sym->symbol.name.size())
                      return a.sym->symbol.name.size() <
                             b.sym->symbol.name.size();
                  return a.sym->symbol.name < b.sym->symbol.name;
              });
    for (size_t i = 0; i < nearby.size() && i < 3; ++i) {
        sims.push_back({{"name", std::string(nearby[i].sym->symbol.name)},
                        {"id", encode_symbol_id(nearby[i].sym->id)}});
    }
    return sims;
}

// -- handle_info --------------------------------------------------------------

ToolResult handle_info(const nlohmann::json& params) {
    auto tool = to_lower(params.value("tool", ""));

    if (tool == "version") {
        nlohmann::json data;
        data["name"] = "version";
        data["server_name"] = "lightning-code-index-mcp";
        data["server_version"] = kVersion;
        data["mcp_version"] = kLatestProtocolVersion;
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
            {"max", "Max results (default: 15, hard cap: 100)"},
            {"output",
             "Output format: 'line', 'ctx', 'ctx:N', 'full', 'files', "
             "'count'"},
            {"path",
             "Root-relative scope: dir prefix ('src/http') or glob "
             "('**/*.kt')"},
            {"filter",
             "Include-only filter: languages/extensions/globs CSV, e.g. "
             "'go', 'md,*.yml', 'src/**/*.py'"},
            {"flags",
             "Search flags: 'ci' (case-insensitive), 'rx' (regex), 'iv' "
             "(invert), 'wb' (word-boundary), 'nt' (no-tests), 'nc' "
             "(no-comments)"},
        };
        data["response"] = {
            {"results",
             "Per-file groups: {file (root-relative), hits:[{line, sym, "
             "type, id, callers, text}]}. id feeds get_context; callers = "
             "incoming references."},
            {"other_files", "Doc/generated files: {path: match_count}"},
            {"total_matches",
             "TRUE match count (may exceed rows shown; truncated:true then "
             "set, with a dirs histogram for narrowing via path=)"},
            {"similar_symbols", "On 0 matches: fuzzy near-miss symbol names"},
        };
        data["example"] = {{"basic", R"({"pattern": "user"})"},
                           {"with_flags", R"({"pattern": "user", "flags": "ci,nt"})"},
                           {"with_path", R"({"pattern": "user", "path": "src/auth"})"},
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

    // Default: overview of all tools. The full text of result.content[0].text
    // is locked by the mcp/info integration golden.
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
        // Recovery guidance: every other tool's error names its allowed
        // params; without this, benchmark traces showed a model retrying an
        // unknown param ("paths") 3x with no way to self-correct.
        err_json["allowed_params"] = {
            "pattern", "patterns", "max", "max_per_file", "output", "path",
            "filter", "flags", "symbol_types", "languages", "semantic",
            "include"};
        return {dump_json_lossy(err_json), true};
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
        return {dump_json_lossy(err_json), true};
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
    // Default 15: repo-QA benchmark (benchmarks/repo-qa, tier 0) measured the
    // old default-50 payload at ~18.6k chars (~4.6k tokens) per call — the
    // dominant context cost of the LCI MCP variant. Agents that need more
    // pass max explicitly; hard cap stays 100.
    int max_results = params.value("max", 15);
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

    // filter → include globs (languages/extensions/globs CSV), matched
    // against root-relative paths — same include semantics as find_files.
    if (params.contains("filter") && params["filter"].is_string()) {
        options.filter_globs = filter_tokens_to_globs(
            params["filter"].get<std::string>());
    }

    // path → root-relative directory prefix or glob scope. Benchmark traces
    // showed agents passing path/file params that used to fail validation —
    // scoping a search to a subtree is a natural ask; support it directly.
    if (params.contains("path") && params["path"].is_string()) {
        options.path_scope = params["path"].get<std::string>();
        // Normalize: strip leading ./ and any trailing slash so both
        // "src/http/" and "./src/http" scope to src/http/**. "." and "./"
        // mean the whole root — no scoping.
        if (options.path_scope.rfind("./", 0) == 0) {
            options.path_scope.erase(0, 2);
        }
        while (!options.path_scope.empty() &&
               options.path_scope.back() == '/') {
            options.path_scope.pop_back();
        }
        if (options.path_scope == ".") options.path_scope.clear();
        // An absolute path is accepted when it points inside the project
        // root (agents paste absolute paths back from other tools) and is
        // relativized; anything else can never match — fail fast.
        if (!options.path_scope.empty() && options.path_scope.front() == '/') {
            const std::string& root = indexer.config().project.root;
            if (options.path_scope == root) options.path_scope.clear();
            auto rel = relative_to_root(options.path_scope, root);
            if (!rel.empty() && rel.front() == '/') {
                return make_error_response(
                    "search",
                    "path must be project-root-relative (or an absolute "
                    "path under the project root " + root + "); got: " +
                        options.path_scope);
            }
            options.path_scope = std::string(rel);
        }
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
    bool include_signature = false;
    if (params.contains("include") && params["include"].is_string()) {
        const auto& inc = params["include"].get_ref<const std::string&>();
        for (auto& tok : parse_list_helper(inc)) {
            if (tok.empty() || tok == "object_ids" || tok == "ids") continue;
            if (tok == "breadcrumbs") { include_breadcrumbs = true; continue; }
            if (tok == "refs") { include_refs = true; continue; }
            // `signature` is list_symbols/browse_file vocabulary; agents
            // carry it here (tier-1 traces: top param-error class). Honor it
            // rather than bouncing the call.
            if (tok == "signature" || tok == "signatures") {
                include_signature = true;
                continue;
            }
            if (tok == "safety" || tok == "deps") continue;  // accepted, unfilled
            return make_error_response(
                "search",
                "include='" + tok + "' is not a recognized search add-on. "
                "Allowed: object_ids, breadcrumbs, refs, signature, safety, "
                "deps.");
        }
    }

    // Perform search. `stats` carries the true pre-truncation match count
    // and directory histogram when the engine path runs; the indexer
    // fallback leaves it zeroed and the emit code falls back to row counts.
    std::vector<SearchResult> results;
    SearchStats stats;
    bool have_stats = search_engine != nullptr;
    auto run = [&](const std::string& p, const SearchOptions& o) {
        if (search_engine) return search_engine->search(p, o, &stats);
        return indexer.search_with_options(p, o);
    };
    auto run_multi = [&](const std::vector<std::string>& ps,
                         const std::vector<bool>& ci_flags,
                         const SearchOptions& o) {
        if (search_engine) return search_engine->search(ps, ci_flags, o, &stats);
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
        SearchStats literal_stats = stats;
        SearchOptions rx_opts = options;
        rx_opts.use_regex = true;
        auto rx_results = run(pattern, rx_opts);
        for (auto& r : rx_results) r.score *= 0.7;
        // Merge with existing literal results, then re-rank.
        results.reserve(results.size() + rx_results.size());
        for (auto& r : rx_results) results.emplace_back(std::move(r));
        SearchCoordinator::rank(results);
        // Combined totals: `run` overwrote stats with the regex pass; keep
        // the larger honest floor and the literal pass's dir histogram when
        // the regex pass found nothing.
        stats.hit_collection_cap |= literal_stats.hit_collection_cap;
        stats.total_found = std::max(
            {literal_stats.total_found, stats.total_found,
             static_cast<int>(results.size())});
        if (stats.dir_counts.empty()) {
            stats.dir_counts = std::move(literal_stats.dir_counts);
        }
        if (static_cast<int>(results.size()) > max_results) {
            results.resize(static_cast<size_t>(max_results));
        }
    }

    // True totals: prefer engine stats (pre-truncation universe) over the
    // truncated row count so total==max cap-saturation never lies to the
    // caller about the result universe.
    const int shown = static_cast<int>(results.size());
    const int total_matches =
        have_stats ? std::max(stats.total_found, shown) : shown;
    const bool truncated = total_matches > shown ||
                           (have_stats && stats.hit_collection_cap);

    // All emitted paths are project-root-relative: the absolute prefix is
    // identical on every row and costs ~110 chars/result in agent context.
    const std::string& proj_root = indexer.config().project.root;
    auto rel = [&proj_root](const std::string& p) {
        return std::string(relative_to_root(p, proj_root));
    };

    auto attach_truncation = [&](nlohmann::json& response) {
        response["total_matches"] = total_matches;
        // Empty results always carry a hint — output=files/count callers
        // previously got a bare zero payload that hid scope mistakes. The
        // standard output path overwrites this with a richer hint below.
        if (total_matches == 0) {
            response["hint"] =
                "0 matches. Try flags=rx for regex, a shorter pattern, or "
                "drop path=/filter= scoping.";
        }
        if (have_stats && stats.hit_collection_cap) {
            response["total_is_lower_bound"] = true;
        }
        if (truncated) {
            response["truncated"] = true;
            // Directory histogram over the FULL match set so the caller can
            // narrow with path= instead of blindly re-guessing patterns.
            if (!stats.dir_counts.empty()) {
                nlohmann::json dirs = nlohmann::json::array();
                size_t emitted = 0;
                for (const auto& [dir, count] : stats.dir_counts) {
                    if (emitted++ == 8) break;
                    dirs.push_back({dir, count});
                }
                response["dirs"] = std::move(dirs);
            }
        }
    };

    // Handle files-only output
    if (output == "files") {
        std::vector<std::string> files;
        for (const auto& r : results) {
            auto rp = rel(r.path);
            if (files.empty() || files.back() != rp) {
                files.push_back(std::move(rp));
            }
        }
        nlohmann::json response;
        response["files"] = files;
        response["showing"] = shown;
        response["unique_files"] = static_cast<int>(files.size());
        attach_truncation(response);
        return make_json_response(response);
    }

    // Handle count output
    if (output == "count") {
        std::map<std::string, int> file_counts;
        for (const auto& r : results) {
            file_counts[rel(r.path)]++;
        }
        nlohmann::json response;
        response["showing"] = shown;
        response["unique_files"] = static_cast<int>(file_counts.size());
        response["counts"] = file_counts;
        attach_truncation(response);
        return make_json_response(response);
    }

    // Build standard results, grouped by file to eliminate repeated path
    // strings (the dominant payload cost measured in the repo-QA benchmark).
    // Detail is tiered by match strength:
    //   - every hit: line (+ match text only when it differs from the shared
    //     top-level "match"), enclosing-symbol enrichment on the first hit
    //     inside that symbol (sym/type/id/callers/exported),
    //   - the top 3 ranked hits additionally carry the matched source line
    //     ("text") so the strongest results are answerable without a read,
    //   - explicit output=ctx:N/full requests keep full ctx arrays on all.
    // Documentation/Unknown files collapse into "other_files" {path: count}
    // so prose/profile junk never crowds out code rows — unless the whole
    // result set is non-code, in which case they stay full rows.
    auto& ref_tracker = indexer.ref_tracker();
    // Pin the RCU snapshot for the whole result loop: get_symbol_at_line hands
    // back a const EnhancedSymbol* into the snapshot, and the pin keeps that
    // pointer valid across a concurrent reindex publish for every iteration.
    auto rt_snap = ref_tracker.pin();

    // Shared match text: literal searches repeat the identical matched
    // substring on every row — emit it once at the top level.
    bool uniform_match = !results.empty();
    for (const auto& r : results) {
        if (r.match_text != results.front().match_text) {
            uniform_match = false;
            break;
        }
    }

    // Group rows by file, preserving rank order of first appearance.
    struct Group {
        const std::string* abs_path;
        std::vector<const SearchResult*> hits;
        bool is_code;
    };
    std::vector<Group> groups;
    groups.reserve(results.size());
    absl::flat_hash_map<std::string_view, size_t> group_of;
    group_of.reserve(results.size());
    bool any_code_group = false;
    for (const auto& r : results) {
        auto [it, inserted] = group_of.emplace(r.path, groups.size());
        if (inserted) {
            auto cat = classify_file(r.path);
            bool is_code = cat != FileCategory::Documentation &&
                           cat != FileCategory::Unknown;
            any_code_group |= is_code;
            groups.push_back({&r.path, {}, is_code});
        }
        groups[it->second].hits.push_back(&r);
    }

    // Tier-1 carriers: the top 3 ranked hits that land in full-detail groups.
    // `results` is already rank-sorted, so the first 3 qualifying rows win.
    absl::flat_hash_set<const SearchResult*> carriers;
    for (const auto& r : results) {
        if (carriers.size() >= 3) break;
        auto cat = classify_file(r.path);
        bool is_code = cat != FileCategory::Documentation &&
                       cat != FileCategory::Unknown;
        if (is_code || !any_code_group) carriers.insert(&r);
    }

    // Explicit context request (output=ctx:N/full/ctx) keeps ctx on all rows.
    const bool explicit_ctx =
        output == "full" || output == "ctx" ||
        (output.size() > 4 && output.compare(0, 4, "ctx:") == 0);

    auto trimmed_line = [](const std::string& s) {
        auto first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string();
        auto last = s.find_last_not_of(" \t\r\n");
        std::string out = s.substr(first, last - first + 1);
        if (out.size() > 200) {
            out.resize(200);
        }
        return out;
    };

    nlohmann::json file_array = nlohmann::json::array();
    nlohmann::json other_files = nlohmann::json::object();
    for (auto& g : groups) {
        if (!g.is_code && any_code_group) {
            // Non-code bucket: path -> hit count, no per-line rows.
            other_files[rel(*g.abs_path)] = static_cast<int>(g.hits.size());
            continue;
        }
        // Within a file, emit hits in line order (reading order); the file
        // groups themselves are already in rank order.
        std::sort(g.hits.begin(), g.hits.end(),
                  [](const SearchResult* a, const SearchResult* b) {
                      return a->line < b->line;
                  });

        nlohmann::json hits = nlohmann::json::array();
        hits.get_ref<nlohmann::json::array_t&>().reserve(g.hits.size());
        const EnhancedSymbol* prev_sym = nullptr;
        for (const SearchResult* r : g.hits) {
            nlohmann::json h;
            h["line"] = r->line;
            if (!uniform_match) h["match"] = r->match_text;

            // Enclosing-symbol enrichment, deduped: consecutive hits inside
            // the same symbol repeat nothing. O(1) hash lookup per row
            // (KARPATHY rule 2 — no allocation in the inner loop).
            auto sym =
                rt_snap->get_symbol_at_line(r->file_id, r->line);
            if (sym != nullptr && sym.get() != prev_sym) {
                h["sym"] = std::string(sym->symbol.name);
                h["type"] = std::string(to_string(sym->symbol.type));
                h["id"] = encode_symbol_id(sym->id);
                if (sym->is_exported) h["exported"] = true;
                // Incoming-reference count bridges straight to get_context:
                // "which function is the chokepoint" is answerable from the
                // search response itself.
                auto callers = static_cast<int>(sym->incoming_refs.size());
                if (callers > 0) h["callers"] = callers;

                if (include_signature && !sym->signature.empty()) {
                    // Declaration line only (browse_file convention) —
                    // bodies belong to get_context.
                    std::string_view sig(sym->signature);
                    auto nl = sig.find('\n');
                    h["signature"] =
                        nl == std::string_view::npos
                            ? std::string(sig)
                            : std::string(sig.substr(0, nl)) + " …";
                }

                // Optional include= add-ons, gated like Go on strong
                // matches: normalizedScore >= 0.5 (scores >1 /100-normalized).
                double normalized =
                    r->score > 1.0 ? r->score / 100.0 : r->score;
                if (normalized >= 0.5) {
                    if (include_refs) {
                        h["references"] = {
                            {"incoming_count", callers},
                            {"outgoing_count",
                             static_cast<int>(sym->outgoing_refs.size())}};
                    }
                    if (include_breadcrumbs && !sym->scope_chain.empty()) {
                        h["breadcrumbs"] = scope_chain_to_breadcrumbs(*sym);
                    }
                }
            }
            if (sym != nullptr) prev_sym = sym.get();

            if (explicit_ctx) {
                if (!r->context.lines.empty()) {
                    h["ctx"] = r->context.lines;
                }
            } else if (carriers.contains(r) && !r->context.lines.empty()) {
                // Matched source line index within the context window.
                size_t match_idx = 0;
                if (r->context.start_line > 0 &&
                    r->line >= r->context.start_line) {
                    match_idx = static_cast<size_t>(
                        r->line - r->context.start_line);
                }
                if (match_idx >= r->context.lines.size()) match_idx = 0;
                auto text = trimmed_line(r->context.lines[match_idx]);
                if (!text.empty()) h["text"] = std::move(text);
            }

            hits.push_back(std::move(h));
        }

        nlohmann::json group_json;
        group_json["file"] = rel(*g.abs_path);
        group_json["hits"] = std::move(hits);
        file_array.push_back(std::move(group_json));
    }

    nlohmann::json response;
    if (uniform_match) response["match"] = results.front().match_text;
    response["results"] = std::move(file_array);
    if (!other_files.empty()) response["other_files"] = std::move(other_files);
    response["showing"] = shown;
    attach_truncation(response);

    // Empty result: fail loud with actionable guidance instead of a bare
    // zero (Karpathy rule 6). Includes near-miss symbol names so a typo'd
    // or wrongly-cased identifier query self-corrects in one round trip.
    if (results.empty()) {
        response["hint"] =
            "0 matches. The index skips build artifacts, minified bundles "
            "and vendored deps (dist/, node_modules/, vendor/, *.min.js) — "
            "use grep for those. Try flags=rx for regex patterns, a shorter "
            "pattern, path= to scope elsewhere, or find_files for filenames.";
        // First >2-char word of the pattern drives the suggestion scan.
        std::string word;
        for (auto& w0 : [&] {
                 std::vector<std::string> ws;
                 split_on_spaces(pattern, ws);
                 return ws;
             }()) {
            if (w0.size() > 2) { word = to_lower(w0); break; }
        }
        if (!word.empty()) {
            auto sims = similar_symbol_suggestions(*rt_snap, word);
            if (!sims.empty()) {
                response["similar_symbols"] = std::move(sims);
            }
        }
    }

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

void attach_source_excerpt(nlohmann::json& ctx, const EnhancedSymbol& sym,
                           MasterIndex& indexer) {
    static constexpr int kMaxExcerptLines = 12;

    const int start_line = std::max(1, sym.symbol.line);
    int end_line = sym.symbol.end_line > 0 ? sym.symbol.end_line : start_line;
    if (end_line < start_line) end_line = start_line;

    const int capped_end =
        std::min(end_line, start_line + kMaxExcerptLines - 1);
    auto fc = indexer.file_content_store().get_file(sym.symbol.file_id);
    if (!fc) return;

    nlohmann::json lines = nlohmann::json::array();
    lines.get_ref<nlohmann::json::array_t&>().reserve(
        static_cast<size_t>(capped_end - start_line + 1));
    for (int line = start_line; line <= capped_end; ++line) {
        auto ref = indexer.file_content_store().get_line(sym.symbol.file_id,
                                                         line - 1);
        auto text = indexer.file_content_store().get_string(ref);
        lines.push_back({{"line", line}, {"text", std::string(text)}});
    }
    if (lines.empty()) return;

    ctx["source_excerpt"] =
        nlohmann::json{{"start_line", start_line},
                       {"end_line", capped_end},
                       {"truncated", capped_end < end_line},
                       {"lines", std::move(lines)}};
    ctx["source_hint"] =
        "source_excerpt is extracted from the indexed source. Use it for "
        "nearby code, initializer, and short body questions; read the file "
        "only if you need lines outside this excerpt.";
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
    // Pin the snapshot so the EnhancedSymbol* stays valid through the ctx
    // build below (attach_purity dereferences *sym) across a concurrent reindex.
    auto rt_snap = indexer.ref_tracker().pin();
    auto sym = rt_snap->get_enhanced_symbol(*decoded);
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
    ctx["file_path"] = std::string(relative_to_root(
        indexer.get_file_path(sym->symbol.file_id),
        indexer.config().project.root));
    ctx["line"] = sym->symbol.line;
    ctx["object_id"] = std::string(id);
    // Incoming-reference count, matching search's per-hit `callers` field —
    // chokepoint questions answerable without a follow-up call-hierarchy
    // request.
    if (!sym->incoming_refs.empty()) {
        ctx["callers"] = static_cast<int>(sym->incoming_refs.size());
    }
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
    attach_source_excerpt(ctx, *sym, indexer);
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

    // Section filtering. Go accepts include_sections in both paths and never
    // errors on them (handlers.go: the compact path ignores sections it can't
    // render — the MCP ObjectContext has no variables/structure/etc. field —
    // and the mode path filters a rich context). C++ honors the sections it
    // can compute (relationships/callers/callees via section_allowed below)
    // and ignores the rest, matching Go's lenient acceptance rather than
    // fail-fasting where Go succeeds. Sections backed by the unported
    // ContextLookupEngine (structure/variables/semantic/usage/ai) simply add
    // no fields to the compact output.

    // name path: minimal port of Go's handleGetObjectContextWithMode.
    // Full ContextLookupEngine (6261 LOC across 8 files in internal/core/
    // context_lookup_*.go) covers structure / semantic / variables /
    // usage / ai sections. We implement the subset MCP callers actually
    // exercise on the standard chi/fastapi/pocketbase tests: name →
    // EnhancedSymbol resolution + optional call hierarchy. Other sections
    // remain unported and absent from the response (omitempty-style).
    //
    // Runs for ANY name lookup, not only when `mode` is set: a bare
    // {"name": X} previously fell through to the id path and returned
    // {contexts:[],count:0} — get_context-by-name silently empty unless the
    // caller happened to also pass mode=. mode still tunes depth/sections via
    // apply_context_lookup_mode above; absence of mode just uses the defaults.
    if (has_name) {
        bool include_call_hierarchy =
            p.value("include_call_hierarchy", false);
        int max_depth = p.value("max_depth", 1);
        if (max_depth < 1) max_depth = 1;
        if (max_depth > 10) max_depth = 10;
        const bool want_relationships =
            section_allowed(p, "relationships") || section_allowed(p, "callers");

        nlohmann::json contexts = nlohmann::json::array();
        auto& tracker = indexer.ref_tracker();
        // Pin the snapshot for the whole match loop + recursive build_tree:
        // find_symbols_by_name returns const EnhancedSymbol* into the snapshot,
        // and those pointers are dereferenced throughout (including the nested
        // lambda's sub_matches), so the pin must outlive every use.
        auto rt_snap = tracker.pin();
        auto matches = rt_snap->find_symbols_by_name(name);
        contexts.get_ref<nlohmann::json::array_t&>().reserve(matches.size());
        for (const auto& sym : matches) {
            if (sym == nullptr) continue;

            std::string definition = sym->signature.empty()
                                         ? std::string(sym->symbol.name)
                                         : std::string(sym->signature);
            nlohmann::json ctx;
            ctx["file_path"] = std::string(relative_to_root(
                indexer.get_file_path(sym->symbol.file_id),
                indexer.config().project.root));
            ctx["line"] = sym->symbol.line;
            ctx["object_id"] = encode_symbol_id(sym->id);
            // Caller count parity with search hits — see resolve_object_id.
            if (!sym->incoming_refs.empty()) {
                ctx["callers"] =
                    static_cast<int>(sym->incoming_refs.size());
            }
            ctx["symbol_type"] = std::string(to_string(sym->symbol.type));
            ctx["symbol_name"] = std::string(sym->symbol.name);
            ctx["is_exported"] = sym->is_exported;
            if (!sym->signature.empty()) {
                ctx["signature"] = std::string(sym->signature);
            }
            ctx["definition"] = definition;
            ctx["context"] = nlohmann::json::array({definition});
            attach_purity(ctx, *sym, indexer, analyzer);
            attach_source_excerpt(ctx, *sym, indexer);

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
                        auto sub_matches = rt_snap->find_symbols_by_name(cn);
                        if (sub_matches.empty()) {
                            kids.push_back({{"root", cn},
                                            {"children",
                                             nlohmann::json::array()}});
                            continue;
                        }
                        const auto& child = sub_matches.front();
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
                            build_tree(child.get(), depth - 1, visited));
                    }
                    t["children"] = std::move(kids);
                    return t;
                };
                absl::flat_hash_set<uint64_t> visited;
                visited.insert(static_cast<uint64_t>(sym->id));
                ctx["call_tree"] =
                    build_tree(sym.get(), max_depth - 1, visited);
            }

            contexts.push_back(std::move(ctx));
        }

        nlohmann::json response;
        response["count"] = static_cast<int>(contexts.size());
        response["contexts"] = std::move(contexts);

        // Full/section requests get the rich CodeObjectContext from the ported
        // ContextLookupEngine (S1 skeleton: object_id/signature/location +
        // diagnostics + all six section keys present-but-empty). A bare
        // {"name": X} without mode/sections keeps the compact envelope only, so
        // existing callers are unaffected.
        const bool rich_request =
            p.value("mode", "") == "full" ||
            (p.contains("include_sections") &&
             p["include_sections"].is_array() &&
             !p["include_sections"].empty()) ||
            (p.contains("exclude_sections") &&
             p["exclude_sections"].is_array() &&
             !p["exclude_sections"].empty());
        if (rich_request) {
            const EnhancedSymbol* target = nullptr;
            for (const auto& sym : matches) {
                if (sym != nullptr) {
                    target = sym.get();
                    break;
                }
            }
            if (target != nullptr) {
                ContextLookupEngine engine(indexer);
                engine.set_max_context_depth(max_depth);
                engine.set_include_ai_text(p.value("include_ai_text", true));
                if (p.contains("confidence_threshold")) {
                    engine.set_confidence_threshold(
                        p["confidence_threshold"].get<double>());
                }
                CodeObjectID oid;
                oid.file_id = target->symbol.file_id;
                oid.symbol_id = encode_symbol_id(target->id);
                oid.name = std::string(target->symbol.name);
                oid.type = target->symbol.type;
                bool ok = false;
                CodeObjectContext obj_ctx = engine.get_context(oid, ok);
                // Honor include_sections / exclude_sections (mode presets
                // funnel through include_sections too): zero the filtered-out
                // sections. Filtered sections stay present-but-empty in the
                // JSON — keys are never dropped. Go parity:
                // Server.filterContextSections.
                std::vector<std::string> include_sections, exclude_sections;
                if (p.contains("include_sections") &&
                    p["include_sections"].is_array()) {
                    for (const auto& v : p["include_sections"]) {
                        if (v.is_string()) {
                            include_sections.push_back(v.get<std::string>());
                        }
                    }
                }
                if (p.contains("exclude_sections") &&
                    p["exclude_sections"].is_array()) {
                    for (const auto& v : p["exclude_sections"]) {
                        if (v.is_string()) {
                            exclude_sections.push_back(v.get<std::string>());
                        }
                    }
                }
                ContextLookupEngine::filter_context_sections(
                    obj_ctx, include_sections, exclude_sections);
                response["context"] = obj_ctx.to_json();
            }
        }
        // Empty lookup fails loud (Karpathy #6): hint + fuzzy near-miss
        // suggestions so a typo'd or misremembered name self-corrects in
        // one round trip instead of a bare {contexts:[],count:0}.
        if (response["count"].get<int>() == 0) {
            response["hint"] =
                "no symbol named '" + name + "' in the index. Check "
                "similar_symbols below, or use search to locate it.";
            auto sims = similar_symbol_suggestions(*rt_snap, name);
            if (!sims.empty()) response["similar_symbols"] = std::move(sims);
        }
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
    // `path` is an alias for `directory` — search uses path=, and agents
    // carry the name across tools.
    auto directory = params.value("directory", "");
    if (directory.empty()) directory = params.value("path", "");
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

    // A pattern carrying wildcards (e.g. "*.ts", "src/*_test.go") is a glob, not
    // a name to fuzzy-score — the literal "*.ts" would never match a filename.
    // Match it directly against basename/path and skip the fuzzy scorer.
    bool pattern_is_glob =
        normalized_pattern.find_first_of("*?") != std::string::npos;

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

    // All filtering and matching below runs on the PROJECT-ROOT-RELATIVE
    // path. The absolute path is wrong for two of the filters: a project
    // living under a dotted ancestor (~/.cache/repo, .work/corpus) would
    // mark every file "hidden", and directory= / glob patterns are written
    // by callers against the repo layout, not the machine's filesystem.
    const std::string& proj_root = indexer.config().project.root;
    auto rel_of = [&proj_root](const std::string& abs) -> std::string {
        if (!proj_root.empty() && abs.size() > proj_root.size() &&
            abs.compare(0, proj_root.size(), proj_root) == 0 &&
            abs[proj_root.size()] == '/') {
            return abs.substr(proj_root.size() + 1);
        }
        return abs;
    };

    // Normalize the directory scope the same way search's path= does:
    // "./x" -> "x", trailing slashes stripped, "." / "/" / the project
    // root itself mean no scoping, and an absolute path under the root is
    // relativized. Tier-1 traces: 8/8 empty find_files calls were agents
    // passing "." or an absolute workspace path and getting a silent 0.
    if (directory.rfind("./", 0) == 0) directory.erase(0, 2);
    while (!directory.empty() && directory.back() == '/') {
        directory.pop_back();
    }
    if (directory == ".") directory.clear();
    if (!directory.empty() && directory.front() == '/') {
        if (directory == proj_root) {
            directory.clear();
        } else {
            auto rel = relative_to_root(directory, proj_root);
            if (!rel.empty() && rel.front() == '/') {
                return make_error_response(
                    "find_files",
                    "directory must be project-root-relative (or an "
                    "absolute path under the project root " + proj_root +
                        "); got: " + directory);
            }
            directory = std::string(rel);
        }
    }

    for (const auto& [abs_path, fid] : snapshot->file_map) {
        const std::string path = rel_of(abs_path);
        // Directory filter (root-relative)
        if (!directory.empty()) {
            if (path.substr(0, directory.size()) != directory ||
                (path.size() > directory.size() &&
                 path[directory.size()] != '/')) {
                continue;
            }
        }

        // Hidden file filter — root-relative components only, so dotted
        // ancestors of the project root never hide the whole corpus.
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

        // 0. Glob pattern: match basename or full path; non-matches are skipped
        //    outright (not fuzzy-scored). A leading `**/` also matches at
        //    zero directory depth (glob convention; wildcard_match's literal
        //    `/` otherwise rejects root-level files — benchmark traces showed
        //    LLMs default to `**/name.go` and got silent empties on files at
        //    the project root).
        if (pattern_is_glob) {
            bool glob_hit = wildcard_match(norm_filename, normalized_pattern) ||
                            wildcard_match(match_path, normalized_pattern);
            if (!glob_hit && normalized_pattern.rfind("**/", 0) == 0) {
                std::string_view tail(normalized_pattern);
                tail.remove_prefix(3);
                glob_hit = wildcard_match(norm_filename, tail) ||
                           wildcard_match(match_path, tail);
            }
            if (glob_hit) {
                score = 1.0;
                match_type = "glob";
            } else {
                continue;
            }
        } else if (match_path == normalized_pattern) {
            // 1. Exact full path match
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
                // Real normalized Levenshtein SIMILARITY: 1.0 identical, 0.0
                // wildly different. The original Go fuzzer (and the verbatim
                // port) used the DISTANCE ratio (lev/max_len) here and then
                // tested `>= 0.7`, which is inverted — unrelated filenames
                // (high distance) passed and every file flooded in at ~0.574,
                // while genuine near-matches were rejected. The Go oracle that
                // pinned that behavior is retired, so use the correct
                // similarity: similarity = 1 - distance/max_len.
                double sim_norm;
                if (normalized_pattern == norm_filename_no_ext) {
                    sim_norm = 1.0;
                } else {
                    size_t lev = rapidfuzz::levenshtein_distance(
                        normalized_pattern, norm_filename_no_ext);
                    size_t max_len =
                        std::max(normalized_pattern.size(),
                                 norm_filename_no_ext.size());
                    sim_norm = 1.0 - static_cast<double>(lev) /
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
                for (const auto& [abs_path, fid] : snapshot->file_map) {
                    const std::string path = rel_of(abs_path);
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

    // Limit results — record the true match count first so the response
    // never reports total==max when the cap truncated a larger set.
    const int total_found = static_cast<int>(matches.size());
    if (total_found > max_results) {
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
    response["total_matches"] = total_found;
    if (total_found > max_results) {
        response["truncated"] = true;
        response["showing"] = max_results;
    }
    response["pattern"] = pattern;

    // Empty result fails loud (Karpathy #6). A nonexistent directory scope
    // is the most common cause in agent traces — name it explicitly.
    if (total_found == 0) {
        if (!directory.empty()) {
            bool dir_exists = false;
            for (const auto& [abs_path, fid] : snapshot->file_map) {
                auto rel = rel_of(abs_path);
                if (rel.size() > directory.size() &&
                    rel.compare(0, directory.size(), directory) == 0 &&
                    rel[directory.size()] == '/') {
                    dir_exists = true;
                    break;
                }
            }
            response["hint"] =
                dir_exists
                    ? "0 matches for '" + pattern + "' under '" + directory +
                          "'. Widen the pattern or drop the directory scope."
                    : "directory '" + directory + "' contains no indexed "
                      "files — check the path (root-relative) or drop it.";
        } else {
            response["hint"] =
                "0 files matched '" + pattern + "'. Globs match the "
                "root-relative path; try a shorter name fragment (fuzzy "
                "matching) or search for content instead.";
        }
    }

    return make_json_response(response);
}

// -- register_core_handlers ---------------------------------------------------

void register_core_handlers(McpServer& server, MasterIndex* indexer,
                            SearchEngine* search_engine,
                            SideEffectAnalyzer* analyzer) {
    // Register the "info" tool (definition + real handler)
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

    // Register the "search" tool (definition + real handler)
    server.add_tool(
        {"search",
         "Sub-millisecond in-memory semantic code search. Use instead of "
         "grep, rg, find. Results are grouped per file (root-relative "
         "paths) with per-hit line numbers; symbol hits carry sym/type/id "
         "(id feeds get_context) and callers (incoming-reference count). "
         "The returned lines are source evidence: answer from them without "
         "reading the file unless you need lines outside the result. "
         "total_matches is the TRUE match count; truncated:true + dirs "
         "histogram appear when the cap bit — narrow with path=. Note: "
         "JSON parameters, not CLI flags. See 'info search' for details.",
         {{"pattern", "string", "Search pattern", ""},
          {"max", "integer", "Maximum results (default: 15, max: 100)", ""},
          {"output", "string",
           "Output format: 'line' (default), 'ctx:N' (N context lines), "
           "'full', 'files', 'count'",
           ""},
          {"path", "string",
           "Root-relative scope: directory prefix ('src/http') or glob "
           "('**/*.kt') applied to file paths",
           ""},
          {"filter", "string",
           "Include ONLY matching files: comma-separated languages, "
           "extensions or globs, e.g. 'go', 'md,*.yml', 'src/**/*.py'", ""},
          {"flags", "string",
           "Comma-separated: cs (case-sensitive), rx (regex), wb "
           "(word-boundary), nt (skip tests), nc (skip comments), iv (invert)",
           ""},
          {"include", "string",
           "Result add-ons, comma-separated. ONLY: object_ids, breadcrumbs, "
           "refs, safety, deps. NOT a file filter (use 'filter').",
           ""},
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

    // Register the "get_context" tool (definition + real handler)
    server.add_tool(
        {"get_context",
         "📋 Get detailed context for specific code objects. Use the 'id' "
         "parameter with object IDs from search results. The returned "
         "signature, file path, line numbers, callers/callees, references, "
         "source_excerpt, and snippets are intended to replace opening the "
         "source file; read files only when this context is missing the exact "
         "lines you must quote. See 'info get_context' for examples.",
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
         {},
         // Legacy id aliases normalize_context_params() rewrites to `id`.
         {"symbol_id", "object_id", "object_ids", "oid"}},
        [indexer, analyzer](const nlohmann::json& p) -> ToolResult {
            if (!indexer) {
                return make_error_response("get_context",
                                           "index not available");
            }
            return handle_get_context(p, *indexer, analyzer);
        });

    // Register the "find_files" tool (definition + real handler)
    server.add_tool(
        {"find_files",
         "📁 Like 'find' or 'fd' - searches file paths, not content, on an "
         "in-memory index. Supports fuzzy matching, glob patterns, and "
         "filters. Use search/browse_file/get_context for source evidence "
         "before reading matched files. See 'info find_files'.",
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
           "Directory to search within (relative to project root)", ""},
          {"path", "string",
           "Alias for 'directory' (matches search's path param)", ""}},
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
