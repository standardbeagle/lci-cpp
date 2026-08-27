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
namespace {

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
/// in place. Unrecognized non-empty modes normalize to "full" (Go's
/// `default:` case); an empty mode is left untouched (compact id path).

}  // namespace

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
    } else {
        // Unrecognized non-empty mode → "full" (Go applyContextLookupMode
        // `default: args.Mode = "full"`, handlers.go:2372-2374). Go dispatches
        // any Mode != "" to the rich mode path, so an unknown mode yields the
        // rich context/metadata/performance envelope, not the compact one.
        // Mirror the default's SHALLOW normalization only: set mode=full so
        // rich_request fires downstream; do NOT apply the "full" branch's
        // max_depth/include_ai_text presets (Go's default case doesn't either).
        params["mode"] = "full";
    }
}

/// Returns true if `section` is allowed by include/exclude_sections (if set).
namespace {

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
    if (sym->incoming_ref_count != 0) {
        ctx["callers"] = static_cast<int>(sym->incoming_ref_count);
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
            if (sym->incoming_ref_count != 0) {
                ctx["callers"] =
                    static_cast<int>(sym->incoming_ref_count);
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
                auto lookup_start = std::chrono::steady_clock::now();
                CodeObjectContext obj_ctx = engine.get_context(oid, ok);
                auto lookup_elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - lookup_start)
                        .count();
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

                // Performance/metadata envelope — C++ port of Go's
                // handleGetObjectContextWithMode result wrapping
                // (createContextMetadata + a per-component timing
                // breakdown). Reuses existing indexer stats + kVersion
                // rather than Go's elaborate memory/index-health probes
                // (rung 4: only what get_context's own scope needs);
                // total_time_ms/component timings are non-deterministic
                // wall-clock and belong under a golden's timed/ignore tiers.
                auto stats = indexer.get_stats();
                response["metadata"] = {
                    {"processed_files", stats.total_files},
                    {"index_size", stats.total_symbols},
                    {"server_version", std::string(kVersion)},
                };
                // component_breakdown is an equal-distribution PLACEHOLDER,
                // bug-for-bug with Go's handleGetObjectContextWithMode
                // (internal/mcp/handlers.go:2297-2299) — the reference divides
                // the single wall-clock total by the component count and emits
                // that same value for all seven fields; it does NO real
                // per-section measurement. per_component_time_ms centralizes
                // and documents that fabrication (reference-port rule 5) so it
                // is a recorded divergence-from-correct, not stray synthesized
                // data. Do not replace with independent timing without an
                // upstream Go change — it would break the parity golden.
                auto per_component =
                    ContextLookupEngine::per_component_time_ms(
                        lookup_elapsed_ms);
                response["performance"] = {
                    {"total_time_ms", lookup_elapsed_ms},
                    {"component_breakdown",
                     {{"basic_info_time", per_component},
                      {"relationships_time", per_component},
                      {"variables_time", per_component},
                      {"semantic_time", per_component},
                      {"structure_time", per_component},
                      {"usage_time", per_component},
                      {"ai_time", per_component}}},
                };
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


}  // namespace mcp
}  // namespace lci
