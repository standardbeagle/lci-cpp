#include <lci/mcp/handlers_core.h>

#include <lci/mcp/handlers_core_shared.h>

#include <algorithm>
#include <filesystem>
#include <cctype>
#include <chrono>
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
                          // Unknown serializes as "" -- the exact bytes
                          // the old always-present string field emitted
                          // for non-file scopes.
                          {"language", s.language_id == LangId::Unknown
                                           ? std::string_view{}
                                           : to_string(s.language_id)},
                          {"visibility", std::string(infer_scope_visibility(s))}});
    }
    return crumbs;
}

}  // namespace

/// Language-name → file-extension table, shared by the `languages[]`
/// include filter, the `filter` token translation, and find_files'
/// `filter` (declared in handlers_core_shared.h). Same table as Go
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

namespace {

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

    // (?i): extension match is case-insensitive so `.CPP` / `.Go` files do
    // not escape a languages[] filter (extensions in the table are lowercase).
    std::string out;
    out.reserve(exts.size() * 5 + 12);
    out.append("(?i)\\.(");
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
    // An unknown name ("golang") used to expand to nothing, which silently
    // DROPPED the filter and searched every language — fail fast instead
    // (karpathy rule 6).
    if (params.contains("languages") && params["languages"].is_array()) {
        std::vector<std::string> langs;
        langs.reserve(params["languages"].size());
        const auto& table = language_ext_table();
        for (const auto& v : params["languages"]) {
            if (!v.is_string()) continue;
            auto lang = v.get<std::string>();
            if (table.find(to_lower(lang)) == table.end()) {
                std::string known;
                for (const auto& [name, exts] : table) {
                    if (!known.empty()) known += ", ";
                    known += name;
                }
                return make_error_response(
                    "search", "unknown language '" + lang +
                                  "'; known languages: " + known);
            }
            langs.push_back(std::move(lang));
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
        // One generic spelling at the boundary: indexed paths and every
        // comparison below use '/', so a Windows caller's "internal\\api"
        // or "C:\\repo\\internal" has to be folded here.
        std::replace(options.path_scope.begin(), options.path_scope.end(),
                     '\\', '/');
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
        // An absolute scope reads "/repo/internal" on POSIX but
        // "C:/repo/internal" on Windows, so front()=='/' recognised only the
        // first and a Windows caller's absolute path fell through as a
        // literal prefix that matched nothing.
        if (!options.path_scope.empty() &&
            std::filesystem::path(options.path_scope).is_absolute()) {
            std::string root = indexer.config().project.root;
            std::replace(root.begin(), root.end(), '\\', '/');
            if (options.path_scope == root) {
                options.path_scope.clear();
            } else {
                auto rel = relative_to_root(options.path_scope, root);
                // Unchanged means it does not live under the project root.
                if (rel.size() == options.path_scope.size()) {
                    return make_error_response(
                        "search",
                        "path must be project-root-relative (or an absolute "
                        "path under the project root " + root + "); got: " +
                            options.path_scope);
                }
                options.path_scope = std::string(rel);
            }
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
        // Merge with existing literal results, dedupe (a line the literal
        // pass already matched keeps its full-score copy; the 0.7-scaled
        // regex duplicate is dropped), then re-rank.
        results = SearchCoordinator::merge(std::move(results),
                                           std::move(rx_results));
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

    // Query rejected before any file was scanned (empty / over-long
    // pattern). The engine returns an empty vector for that, same as a valid
    // query with no hits, so report the reason instead of a bare 0-match
    // payload.
    if (have_stats && !stats.error.empty()) {
        return make_error_response("search", stats.error);
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
                auto callers = static_cast<int>(sym->incoming_ref_count);
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
                             static_cast<int>(sym->outgoing_ref_count)}};
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


}  // namespace mcp
}  // namespace lci
