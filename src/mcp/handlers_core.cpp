#include <lci/mcp/handlers_core.h>

#include <lci/mcp/handlers_core_shared.h>

#include <algorithm>
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
    return handle_info(params, nullptr);
}

ToolResult handle_info(const nlohmann::json& params,
                       const ToolDefinitionLookup& lookup) {
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

    // "files" is the historic short name for find_files (kept in the
    // overview text); map it before the registry lookup.
    if (tool == "files") tool = "find_files";

    // Registry-derived help for every registered tool: the same definition
    // tools/list serves, so this can never lag the real surface. The
    // hand-written per-tool branches this replaces had drifted (get_context
    // showed 5 of 17 parameters).
    if (lookup) {
        if (const ToolDefinition* def = lookup(tool)) {
            nlohmann::json data;
            data["name"] = def->name;
            data["description"] = def->description;
            nlohmann::json parameters = nlohmann::json::object();
            for (const auto& prop : def->properties) {
                bool required =
                    std::find(def->required.begin(), def->required.end(),
                              prop.name) != def->required.end();
                std::string entry;
                if (required) entry += "REQUIRED: ";
                entry += prop.description;
                entry += " (";
                entry += prop.type;
                if (prop.type == "array" && !prop.items_type.empty()) {
                    entry += " of " + prop.items_type;
                }
                entry += ")";
                parameters[prop.name] = std::move(entry);
            }
            data["parameters"] = std::move(parameters);
            if (!def->aliases.empty()) data["parameter_aliases"] = def->aliases;
            return make_json_response(data);
        }
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
        [&server](const nlohmann::json& p) {
            return handle_info(p, [&server](const std::string& name) {
                return server.find_tool_definition(name);
            });
        },
        // concurrent_ok proof: reads only registered_tools_, which is frozen
        // before run() (add_tool rejects late registration); no other state.
        /*concurrent_ok=*/true);

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
                return make_unavailable_response(
                    "search", "index not available",
                    "retry shortly; the server is still starting or indexing");
            }
            return handle_search(p, *indexer, search_engine);
        },
        // concurrent_ok proof: SearchEngine::search is const with no mutable
        // members; MasterIndex/trigram/postings/reference-tracker reads are
        // RCU snapshots (search_count_ is atomic); handler statics are const
        // magic-statics (flag table, JSON-schema validator, validate() is
        // const). No handler-local shared mutable state.
        /*concurrent_ok=*/true);

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
                return make_unavailable_response(
                    "get_context", "index not available",
                    "retry shortly; the server is still starting or indexing");
            }
            return handle_get_context(p, *indexer, analyzer);
        }
        // exclusive (default): the context-lookup engine + SideEffectAnalyzer
        // path is unaudited for concurrent reads — not proven snapshot-only.
    );

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
                return make_unavailable_response(
                    "find_files", "index not available",
                    "retry shortly; the server is still starting or indexing");
            }
            return handle_find_files(p, *indexer);
        },
        // concurrent_ok proof: reads only indexer.load_snapshot() (atomic
        // file snapshot) and config(); no statics, no mutation.
        /*concurrent_ok=*/true);
}

}  // namespace mcp
}  // namespace lci
