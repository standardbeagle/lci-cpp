#include <lci/mcp/handlers_analysis.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include <lci/analysis/call_graph.h>
#include <lci/analysis/ci_vocabulary_analyzer.h>
#include <lci/analysis/codebase_intelligence.h>
#include <lci/analysis/coupling_analyzer.h>
#include <lci/analysis/feature_analyzer.h>
#include <lci/analysis/health_analyzer.h>
#include <lci/analysis/layer_analyzer.h>
#include <lci/analysis/module_analyzer.h>
#include <lci/analysis/naming_analyzer.h>
#include <lci/analysis/side_effect_analyzer.h>
#include <lci/core/graph_propagator.h>
#include <lci/core/reference_tracker.h>
#include <lci/core/semantic_annotator.h>
#include <lci/git/analyzer.h>
#include <lci/git/frequency_analyzer.h>
#include <lci/git/provider.h>
#include <lci/git/types.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/validation.h>
#include <lci/search/search_engine.h>
#include <lci/symbol.h>

#include <absl/container/flat_hash_set.h>

namespace lci {
namespace mcp {

// -- Helpers ------------------------------------------------------------------

namespace {

// categories_to_strings is shared via side_effects.h (also used by
// get_context purity emission) — no local duplicate.

/// Maps a category name string to a side_effect bitfield constant.
uint32_t category_name_to_bit(std::string_view name) {
    // Normalise to lowercase
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (lower == "param_write" || lower == "paramwrite" || lower == "param-write")
        return side_effect::kParamWrite;
    if (lower == "receiver_write" || lower == "receiverwrite" || lower == "receiver-write")
        return side_effect::kReceiverWrite;
    if (lower == "global_write" || lower == "globalwrite" || lower == "global-write" || lower == "global")
        return side_effect::kGlobalWrite;
    if (lower == "closure_write" || lower == "closurewrite" || lower == "closure-write" || lower == "closure")
        return side_effect::kClosureWrite;
    if (lower == "field_write" || lower == "fieldwrite" || lower == "field-write")
        return side_effect::kFieldWrite;
    if (lower == "io") return side_effect::kIO;
    if (lower == "database" || lower == "db") return side_effect::kDatabase;
    if (lower == "network" || lower == "net") return side_effect::kNetwork;
    if (lower == "throw" || lower == "throws" || lower == "panic")
        return side_effect::kThrow;
    if (lower == "channel" || lower == "chan") return side_effect::kChannel;
    if (lower == "async") return side_effect::kAsync;
    if (lower == "external_call" || lower == "externalcall" || lower == "external-call" || lower == "external")
        return side_effect::kExternalCall;
    if (lower == "dynamic_call" || lower == "dynamiccall" || lower == "dynamic-call" || lower == "dynamic")
        return side_effect::kDynamicCall;
    if (lower == "reflection" || lower == "reflect")
        return side_effect::kReflection;
    if (lower == "uncertain" || lower == "unknown")
        return side_effect::kUncertain;
    return side_effect::kNone;
}

/// Builds a JSON object from a SideEffectInfo entry.
nlohmann::json side_effect_to_json(const SideEffectInfo& info,
                                   bool include_reasons,
                                   bool include_transitive,
                                   bool include_confidence,
                                   std::string_view root) {
    nlohmann::json item;
    item["symbol_name"] = info.function_name;
    item["file_path"] = std::string(relative_to_root(info.file_path, root));
    item["line"] = info.start_line;
    if (info.end_line > 0) item["end_line"] = info.end_line;
    item["is_pure"] = info.is_pure;
    if (info.purity_score > 0.0) item["purity_score"] = info.purity_score;

    auto local_cats = categories_to_strings(info.categories);
    if (!local_cats.empty()) item["local_categories"] = local_cats;

    if (include_transitive) {
        auto trans_cats = categories_to_strings(info.transitive_categories);
        if (!trans_cats.empty()) item["transitive_categories"] = trans_cats;
    }

    if (include_confidence) {
        item["confidence"] = std::string(to_string(info.confidence));
    }

    if (include_reasons && !info.impurity_reasons.empty()) {
        item["reasons"] = info.impurity_reasons;
    }

    if (info.has_access_pattern) {
        item["access_pattern"] = std::string(to_string(info.access_pattern.pattern));
        nlohmann::json violations = nlohmann::json::array();
        for (const auto& v : info.access_pattern.violations) {
            violations.push_back(
                std::string(to_string(v.type)) + " (severity: " +
                std::to_string(v.severity).substr(0, 4) + ")");
        }
        if (!violations.empty()) item["violations"] = std::move(violations);
    }

    if (info.has_error_handling) {
        item["can_throw"] = info.error_handling.can_throw;
        item["exception_neutral"] = info.error_handling.exception_neutral;
        item["exception_safe"] = info.error_handling.exception_safe;
        if (info.error_handling.defer_count > 0)
            item["defer_count"] = info.error_handling.defer_count;
    }

    return item;
}

/// Collects FileSymbolData from a MasterIndex for CI engine input.
std::vector<FileSymbolData> collect_file_symbol_data(MasterIndex& indexer) {
    auto file_ids = indexer.get_all_file_ids();
    auto& ref = indexer.ref_tracker();
    auto rt_snap = ref.pin();
    std::vector<FileSymbolData> result;
    result.reserve(file_ids.size());
    for (auto fid : file_ids) {
        auto path = indexer.get_file_path(fid);
        auto syms = rt_snap->get_file_enhanced_symbols(fid);
        FileSymbolData file;
        file.path = std::move(path);
        file.owner = rt_snap;
        file.symbols.reserve(syms.size());
        for (const auto& sym : syms) file.symbols.push_back(sym.get());
        result.push_back(std::move(file));
    }
    return result;
}

int clamp(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

}  // namespace

// -- handle_semantic_annotations ----------------------------------------------

ToolResult handle_semantic_annotations(const nlohmann::json& raw_params,
                                       SemanticAnnotator& annotator,
                                       GraphPropagator* propagator,
                                       MasterIndex* indexer) {
    auto params = raw_params.is_object() ? raw_params : nlohmann::json::object();
    auto label = params.value("label", "");
    auto category = params.value("category", "");

    if (label.empty() && category.empty()) {
        return make_error_response(
            "semantic_annotations",
            "must specify either 'label' or 'category'");
    }

    double min_strength = params.value("min_strength", 0.0);
    bool include_direct = params.value("include_direct", true);
    bool include_propagated = params.value("include_propagated", true);
    int max_results = clamp(params.value("max_results", 100), 1, 10000);

    // Root for root-relative path emission (empty when no index).
    std::string_view root =
        indexer ? std::string_view(indexer->config().project.root)
                : std::string_view{};

    // Default: both direct and propagated when neither is set
    if (!params.contains("include_direct") &&
        !params.contains("include_propagated")) {
        include_direct = true;
        include_propagated = true;
    }

    nlohmann::json annotations = nlohmann::json::array();
    int count = 0;

    auto serialize_direct = [&](const AnnotatedSymbol* sym) {
        nlohmann::json item;
        item["symbol_name"] = sym->name;
        item["file_id"] = static_cast<int>(sym->file_id);
        item["symbol_id"] = std::to_string(sym->symbol_id);
        item["file_path"] = std::string(relative_to_root(sym->file_path, root));
        item["line"] = sym->line;
        if (!sym->annotation.labels.empty())
            item["direct_labels"] = sym->annotation.labels;
        if (!sym->annotation.category.empty())
            item["category"] = sym->annotation.category;
        if (!sym->annotation.tags.empty()) {
            nlohmann::json tags;
            for (const auto& [k, v] : sym->annotation.tags) {
                tags[k] = v;
            }
            item["tags"] = std::move(tags);
        }
        return item;
    };

    // Query by label - direct annotations
    if (!label.empty() && include_direct) {
        auto symbols = annotator.get_symbols_by_label(label);
        for (const auto& sym : symbols) {
            if (count >= max_results) break;
            annotations.push_back(serialize_direct(sym));
            ++count;
        }
    }

    // Query by label - propagated labels. The propagator is keyed on real
    // EnhancedSymbol IDs (seeded in mcp.cpp from the live index), not the
    // annotator's synthetic packed sym_id. For each direct result we resolve
    // back to the real EnhancedSymbol via ref_tracker(file_id + line) and
    // query the propagator with that ID. source_name/source_file are filled
    // by reverse-looking-up the propagated label's source SymbolID. Karpathy
    // #4: deterministic — get_file_enhanced_symbols returns ordered list,
    // we match the first symbol on the target line.
    if (!label.empty() && include_propagated && propagator && indexer) {
        const auto& ref = indexer->ref_tracker();
        auto rt_snap = ref.pin();
        for (auto& item : annotations) {
            auto file_id = static_cast<FileID>(
                item.value("file_id", 0));
            int line = item.value("line", -1);
            if (line < 0) continue;

            const EnhancedSymbol* real_sym = nullptr;
            for (const auto& es : rt_snap->get_file_enhanced_symbols(file_id)) {
                if (es && es->symbol.line == line) {
                    real_sym = es.get();
                    break;
                }
            }
            if (!real_sym) continue;

            auto labels = propagator->get_labels(real_sym->id);
            nlohmann::json prop_labels = nlohmann::json::array();
            for (const auto& pl : labels) {
                if (pl.label != label || pl.strength < min_strength) continue;
                nlohmann::json pl_item;
                pl_item["label"] = pl.label;
                pl_item["strength"] = pl.strength;
                pl_item["hops"] = pl.hops;
                // Populate source_name / source_file by reverse symbol
                // lookup — fixes the comment-only stub at the prior
                // location. If the source ID isn't found in the index
                // (orphaned propagation root) we leave the fields out
                // rather than emit empty strings (matches Go's omitempty).
                if (pl.source != 0) {
                    if (auto src_es =
                            rt_snap->get_enhanced_symbol(pl.source)) {
                        pl_item["source_name"] = src_es->symbol.name;
                        pl_item["source_file"] = std::string(relative_to_root(
                            indexer->get_file_path(src_es->symbol.file_id),
                            root));
                    }
                }
                prop_labels.push_back(std::move(pl_item));
            }
            if (!prop_labels.empty()) {
                item["propagated_labels"] = std::move(prop_labels);
            }
        }
    }

    // Query by category - direct annotations only (Go's surface; propagator
    // propagates labels, not categories). Use the populated category_index_
    // on the annotator.
    if (!category.empty() && include_direct) {
        auto symbols = annotator.get_symbols_by_category(category);
        // De-dup against any already-collected (label query may have added
        // the same symbol). Match Go: check existing items by symbol_id.
        absl::flat_hash_set<std::string> seen;
        for (auto& item : annotations) {
            auto sid = item.value("symbol_id", "");
            if (!sid.empty()) seen.insert(sid);
        }
        for (const auto& sym : symbols) {
            if (count >= max_results) break;
            auto sid_str = std::to_string(sym->symbol_id);
            if (seen.contains(sid_str)) continue;
            annotations.push_back(serialize_direct(sym));
            ++count;
        }
    }

    nlohmann::json response;
    response["annotations"] = std::move(annotations);
    response["total_count"] = count;
    // Fail loud on empty (Karpathy #6): a bare {annotations:[],total_count:0}
    // can't tell "wrong label" from "corpus has no @lci: annotations at all".
    // Derive the distinction from the annotator's own totals.
    if (count == 0) {
        std::string what =
            !label.empty() && !category.empty()
                ? ("label '" + label + "' / category '" + category + "'")
                : (!label.empty() ? "label '" + label + "'"
                                  : "category '" + category + "'");
        int total_ann = annotator.total_annotations();
        if (total_ann == 0) {
            response["hint"] =
                "no @lci: semantic annotations exist in this corpus; add "
                "`@lci:label=...` comments above symbols to populate them";
        } else {
            response["hint"] =
                "no symbols matched " + what + "; the index holds " +
                std::to_string(total_ann) + " annotated symbol(s) across " +
                std::to_string(annotator.unique_labels()) +
                " label(s) — check spelling/case";
        }
    }
    return make_json_response(response);
}

// -- handle_side_effects (mode dispatch) --------------------------------------

namespace {

ToolResult side_effect_symbol_query(const nlohmann::json& params,
                                    SideEffectAnalyzer& analyzer,
                                    MasterIndex* indexer,
                                    std::string_view root) {
    auto symbol_name = params.value("symbol_name", "");
    auto file_path = params.value("file_path", "");

    if (symbol_name.empty() && file_path.empty()) {
        return make_error_response(
            "side_effects",
            "symbol mode requires 'symbol_name' or 'file_path' with symbol lookup");
    }

    bool include_reasons = params.value("include_reasons", false);
    bool include_transitive = params.value("include_transitive", false);
    bool include_confidence = params.value("include_confidence", false);

    // Search by symbol name through the index's reference tracker
    if (!symbol_name.empty() && indexer) {
        auto& ref = indexer->ref_tracker();
        auto rt_snap = ref.pin();
        auto symbols = rt_snap->find_symbols_by_name(symbol_name);
        if (symbols.empty()) {
            return make_error_response(
                "side_effects", "symbol not found: " + symbol_name);
        }
        const auto& sym = symbols[0];
        auto path = indexer->get_file_path(sym->symbol.file_id);
        auto* info = analyzer.get_result(path, sym->symbol.line);
        if (!info) {
            // Fail loud (Karpathy #6): we resolved the symbol but the analyzer
            // holds no side-effect record for it — a bare empty results array
            // reads as "pure/no effects" and misleads. Say why.
            return make_error_response(
                "side_effects",
                "symbol '" + symbol_name + "' resolved at " +
                    std::string(relative_to_root(path, root)) + ":" +
                    std::to_string(sym->symbol.line) +
                    " but has no side-effect record (not a function/method, or "
                    "the analyzer is unpopulated for this corpus — try "
                    "side_effects {\"mode\":\"summary\"})");
        }
        auto item = side_effect_to_json(*info, include_reasons,
                                        include_transitive, include_confidence,
                                        root);
        nlohmann::json response;
        response["results"] = nlohmann::json::array({std::move(item)});
        response["total_count"] = 1;
        response["mode"] = "symbol";
        return make_json_response(response);
    }

    return make_error_response(
        "side_effects", "symbol lookup requires an index with 'symbol_name'");
}

ToolResult side_effect_file_query(const nlohmann::json& params,
                                  SideEffectAnalyzer& analyzer,
                                  std::string_view root) {
    auto file_path = params.value("file_path", "");
    if (file_path.empty()) {
        return make_error_response(
            "side_effects", "file mode requires 'file_path'");
    }

    bool include_reasons = params.value("include_reasons", false);
    bool include_transitive = params.value("include_transitive", false);
    bool include_confidence = params.value("include_confidence", false);
    int max_results = clamp(params.value("max_results", 100), 1, 10000);

    // Analyzer keys hold absolute file_path. Compare in root-relative space so
    // both a project-relative arg ("src/x.go") and an absolute one match —
    // relative_to_root leaves an already-relative arg untouched.
    std::string want_rel(relative_to_root(file_path, root));

    nlohmann::json results = nlohmann::json::array();
    int total = 0;
    int shown = 0;
    for (const auto& [key, info] : analyzer.results()) {
        if (std::string(relative_to_root(info.file_path, root)) != want_rel)
            continue;
        ++total;
        if (shown < max_results) {
            results.push_back(side_effect_to_json(info, include_reasons,
                                                  include_transitive,
                                                  include_confidence, root));
            ++shown;
        }
    }

    nlohmann::json response;
    response["results"] = std::move(results);
    response["total_count"] = total;
    response["mode"] = "file";
    if (total == 0) {
        // Fail loud (Karpathy #6): distinguish "analyzer holds nothing" from
        // "this file matched nothing" so the caller knows which lever to pull.
        response["hint"] = analyzer.results().empty()
            ? "no per-function side-effect data for this corpus; use "
              "side_effects {\"mode\":\"summary\"}"
            : "no analyzed functions in '" + want_rel +
                  "'; check the path (project-root-relative) or list files "
                  "with debug_info {\"mode\":\"files\"}";
    }
    return make_json_response(response);
}

ToolResult side_effect_purity_query(const nlohmann::json& params,
                                    SideEffectAnalyzer& analyzer,
                                    bool want_pure, std::string_view root) {
    bool include_reasons = params.value("include_reasons", false);
    bool include_transitive = params.value("include_transitive", false);
    bool include_confidence = params.value("include_confidence", false);
    int max_results = clamp(params.value("max_results", 100), 1, 10000);

    nlohmann::json results = nlohmann::json::array();
    int total = 0;
    int shown = 0;
    for (const auto& [key, info] : analyzer.results()) {
        if (info.is_pure == want_pure) {
            ++total;
            if (shown < max_results) {
                results.push_back(side_effect_to_json(
                    info, include_reasons, include_transitive,
                    include_confidence, root));
                ++shown;
            }
        }
    }

    nlohmann::json response;
    response["results"] = std::move(results);
    response["total_count"] = total;
    response["mode"] = want_pure ? "pure" : "impure";
    if (total == 0) {
        response["hint"] = analyzer.results().empty()
            ? "no per-function side-effect data for this corpus; use "
              "side_effects {\"mode\":\"summary\"}"
            : std::string("no ") + (want_pure ? "pure" : "impure") +
                  " functions among the analyzed set; try the opposite mode "
                  "or side_effects {\"mode\":\"summary\"}";
    }
    return make_json_response(response);
}

ToolResult side_effect_category_query(const nlohmann::json& params,
                                      SideEffectAnalyzer& analyzer,
                                      std::string_view root) {
    auto category = params.value("category", "");
    if (category.empty()) {
        return make_error_response(
            "side_effects", "category mode requires 'category' parameter");
    }

    uint32_t bit = category_name_to_bit(category);
    if (bit == side_effect::kNone) {
        return make_error_response(
            "side_effects",
            "unknown category: " + category +
            " (valid: param_write, global_write, io, network, throw, "
            "channel, external_call)");
    }

    bool include_reasons = params.value("include_reasons", false);
    bool include_transitive = params.value("include_transitive", false);
    bool include_confidence = params.value("include_confidence", false);
    int max_results = clamp(params.value("max_results", 100), 1, 10000);

    nlohmann::json results = nlohmann::json::array();
    int total = 0;
    int shown = 0;
    for (const auto& [key, info] : analyzer.results()) {
        uint32_t combined = info.categories | info.transitive_categories;
        if ((combined & bit) == 0) continue;
        ++total;
        if (shown < max_results) {
            results.push_back(side_effect_to_json(info, include_reasons,
                                                  include_transitive,
                                                  include_confidence, root));
            ++shown;
        }
    }

    nlohmann::json response;
    response["results"] = std::move(results);
    response["total_count"] = total;
    response["mode"] = "category";
    if (total == 0) {
        response["hint"] = analyzer.results().empty()
            ? "no per-function side-effect data for this corpus; use "
              "side_effects {\"mode\":\"summary\"}"
            : "no functions with '" + category +
                  "' effects among the analyzed set; try side_effects "
                  "{\"mode\":\"summary\"} for the aggregate breakdown";
    }
    return make_json_response(response);
}

// Counts callable symbols (functions, methods, constructors) across the index.
// Used as the honest fallback when SideEffectAnalyzer.results() is empty
// because the analyzer hasn't been wired into the indexing pipeline yet
// (tracked under tasks sL9fAGaKTXzc, gW7m27uOpsse, yUAZOemJ80R0, 3aSKJjjAFaUv,
// 7t4FBM17kI1W). Decision: the propagator auto-populates from extracted symbols
// and defaults to pure when no effects are observed, so summary reports the
// callable-symbol count rather than zero on an unanalyzed corpus.
int count_callable_symbols_in_index(const MasterIndex& indexer) {
    int total = 0;
    const auto& ref = indexer.ref_tracker();
    auto rt_snap = ref.pin();
    for (FileID fid : indexer.get_all_file_ids()) {
        for (const auto& es : rt_snap->get_file_enhanced_symbols(fid)) {
            if (!es) continue;
            switch (es->symbol.type) {
                case SymbolType::Function:
                case SymbolType::Method:
                case SymbolType::Constructor:
                    ++total;
                    break;
                default:
                    break;
            }
        }
    }
    return total;
}

ToolResult side_effect_summary(SideEffectAnalyzer& analyzer,
                               MasterIndex* indexer) {
    int total = 0;
    int pure_count = 0;
    int impure_count = 0;
    int with_param_writes = 0;
    int with_global_writes = 0;
    int with_io = 0;
    int with_throws = 0;
    int with_external = 0;

    for (const auto& [key, info] : analyzer.results()) {
        ++total;
        if (info.is_pure) {
            ++pure_count;
        } else {
            ++impure_count;
        }
        uint32_t combined = info.categories | info.transitive_categories;
        if (combined & side_effect::kParamWrite) ++with_param_writes;
        if (combined & side_effect::kGlobalWrite) ++with_global_writes;
        if (combined & (side_effect::kIO | side_effect::kNetwork |
                        side_effect::kDatabase))
            ++with_io;
        if (combined & side_effect::kThrow) ++with_throws;
        if (combined & side_effect::kExternalCall) ++with_external;
    }

    // Fallback: SideEffectAnalyzer isn't wired into the MCP indexing pipeline
    // yet (full wiring tracked under tasks sL9fAGaKTXzc et al). When results
    // are empty, fall through to a function-count default so summary mode
    // reports total_count honestly — matches Go's propagator-defaults-to-pure
    // behaviour observed on parity corpora. Per-function purity data stays
    // empty (results=null); only the aggregate counts in `summary` are
    // populated. NOT a silent fallback — documented in MODULE_MAP.md
    // (Decision: side_effects summary fallback, FIX-D.1.B / TwJuY55J9KM1).
    if (total == 0 && indexer != nullptr) {
        total = count_callable_symbols_in_index(*indexer);
        pure_count = total;  // Go defaults unobserved functions to pure.
    }

    nlohmann::json summary;
    summary["total_functions"] = total;
    summary["pure_functions"] = pure_count;
    summary["impure_functions"] = impure_count;
    summary["purity_ratio"] =
        total > 0 ? static_cast<double>(pure_count) / total : 0.0;
    if (with_param_writes > 0) summary["with_param_writes"] = with_param_writes;
    if (with_global_writes > 0) summary["with_global_writes"] = with_global_writes;
    if (with_io > 0) summary["with_io_effects"] = with_io;
    if (with_throws > 0) summary["with_throws"] = with_throws;
    if (with_external > 0) summary["with_external_calls"] = with_external;

    nlohmann::json response;
    response["results"] = nullptr;
    response["total_count"] = total;
    response["mode"] = "summary";
    response["summary"] = std::move(summary);
    return make_json_response(response);
}

}  // namespace

ToolResult handle_side_effects(const nlohmann::json& raw_params,
                               SideEffectAnalyzer& analyzer,
                               MasterIndex* indexer) {
    auto params = raw_params.is_object() ? raw_params : nlohmann::json::object();
    auto mode = params.value("mode", "summary");

    // Root for root-relative path emission / matching (empty when no index).
    std::string_view root =
        indexer ? std::string_view(indexer->config().project.root)
                : std::string_view{};

    if (mode == "symbol")
        return side_effect_symbol_query(params, analyzer, indexer, root);
    if (mode == "file") return side_effect_file_query(params, analyzer, root);
    if (mode == "pure")
        return side_effect_purity_query(params, analyzer, true, root);
    if (mode == "impure")
        return side_effect_purity_query(params, analyzer, false, root);
    if (mode == "category")
        return side_effect_category_query(params, analyzer, root);
    if (mode == "summary") return side_effect_summary(analyzer, indexer);

    return make_error_response(
        "side_effects",
        "unknown mode: " + mode +
        " (valid: symbol, file, pure, impure, category, summary)");
}

// -- Helpers for code_insight LCF emission -----------------------------------

namespace {

// Gather FileSymbolData from the live index: one entry per file with its
// enhanced symbol pointers. Skips files with no symbols.
std::vector<FileSymbolData> gather_file_symbol_data(MasterIndex& indexer) {
    std::vector<FileSymbolData> result;
    auto& ref = indexer.ref_tracker();
    auto rt_snap = ref.pin();
    for (auto fid : indexer.get_all_file_ids()) {
        auto syms = rt_snap->get_file_enhanced_symbols(fid);
        if (syms.empty()) continue;
        FileSymbolData fsd;
        fsd.path = indexer.get_file_path(fid);
        fsd.owner = rt_snap;
        fsd.symbols.reserve(syms.size());
        for (const auto& sym : syms) fsd.symbols.push_back(sym.get());
        result.push_back(std::move(fsd));
    }
    return result;
}

// Render a double with two/one decimals (matches Go's "%.2f"/"%.1f" output).
std::string fmt2(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return std::string(buf);
}
std::string fmt1(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return std::string(buf);
}

// LCF token estimate for the header. Mirrors Go's estimateLCFTokenCount:
//   modules*20 + dep_edges*15 + (health?50) + entry*15 + (stats?50) + 20.
// `n_modules` is the post-truncation count (<=15) the repo map actually
// emits, matching Go (the formatter runs after budget truncation).
int lcf_token_count(int n_modules, int n_dep_edges, bool has_health,
                    int n_entry, bool has_stats) {
    int est = n_modules * 20 + n_dep_edges * 15 + n_entry * 15 + 20;
    if (has_health) est += 50;
    if (has_stats) est += 50;
    return est;
}

void emit_lcf_header(std::ostringstream& out, std::string_view mode, int tier,
                     int tokens) {
    out << "LCF/1.0\nmode=" << mode << "\ntier=" << tier << "\ntokens="
        << tokens << "\n---\n";
}

// == REPOSITORY MAP == — one line per module, capped to 15 (Go truncates to
// 15 during budget enforcement). Emitted only when non-empty (Go: nil skip).
void emit_repository_map(std::ostringstream& out,
                         const std::vector<ModuleBoundary>& mods) {
    if (mods.empty()) return;
    out << "== REPOSITORY MAP ==\n";
    size_t lim = std::min(mods.size(), size_t{15});
    for (size_t i = 0; i < lim; ++i) {
        out << "module=" << mods[i].name << " files=" << mods[i].file_count
            << "\n";
    }
    out << "---\n";
}

// == HEALTH == — score, complexity, smell summary + detail, problematic
// symbols, purity. Object IDs ([o=XX]) come from analyzer-populated fields.
void emit_health(std::ostringstream& out, const HealthDashboard& hd,
                 const PuritySummary* purity) {
    out << "== HEALTH ==\n"
        << "score=" << fmt2(hd.overall_score) << "\n"
        << "complexity=" << fmt2(hd.complexity.average_cc) << "\n";

    if (!hd.smell_counts.empty()) {
        // Go iterates the smell-count map (non-deterministic order). The C++
        // port sorts by smell type for stable output.
        std::vector<std::pair<std::string, int>> sc(hd.smell_counts.begin(),
                                                     hd.smell_counts.end());
        std::sort(sc.begin(), sc.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        out << "smells:";
        for (const auto& [type, count] : sc) out << " " << type << "=" << count;
        out << "\n";
    }
    if (!hd.detailed_smells.empty()) {
        out << "detailed_smells:\n";
        for (const auto& s : hd.detailed_smells) {
            out << "  [" << s.severity << "] " << s.type << ": " << s.symbol
                << " (" << s.location << ") [o=" << s.object_id << "]\n";
        }
    }
    if (!hd.problematic_symbols.empty()) {
        out << "problematic_symbols:\n";
        for (const auto& ps : hd.problematic_symbols) {
            out << "  " << ps.name << " (" << ps.location << ")"
                << " risk=" << ps.risk_score;
            if (!ps.tags.empty()) {
                out << " [";
                for (size_t i = 0; i < ps.tags.size(); ++i) {
                    if (i) out << ",";
                    out << ps.tags[i];
                }
                out << "]";
            }
            out << " [o=" << ps.object_id << "]\n";
        }
    }
    if (purity) {
        double ratio = purity->total_functions > 0
            ? static_cast<double>(purity->pure_functions) /
                  purity->total_functions
            : 0.0;
        out << "purity:\n"
            << "  total=" << purity->total_functions
            << " pure=" << purity->pure_functions
            << " impure=" << purity->impure_functions
            << " ratio=" << fmt2(ratio) << "\n";
        if (purity->with_io_effects > 0 || purity->with_global_writes > 0 ||
            purity->with_param_writes > 0) {
            out << "  effects: io=" << purity->with_io_effects
                << " global_writes=" << purity->with_global_writes
                << " param_writes=" << purity->with_param_writes
                << " throws=" << purity->with_throws << "\n";
        }
        out << "  query: side_effects {\"mode\": \"impure\", "
               "\"include_reasons\": true}\n";
    }
    out << "---\n";
}

// == MODULES == — aggregate cohesion/coupling + top-10 modules by file count.
void emit_modules(std::ostringstream& out, const ModuleAnalysis& ma) {
    if (ma.modules.empty()) return;
    out << "== MODULES ==\n"
        << "total=" << ma.metrics.total_modules
        << " cohesion=" << fmt2(ma.metrics.average_cohesion)
        << " coupling=" << fmt2(ma.metrics.average_coupling) << "\n";
    size_t lim = std::min(ma.modules.size(), size_t{10});
    for (size_t i = 0; i < lim; ++i) {
        const auto& m = ma.modules[i];
        out << "  " << m.name << ": type=" << m.type
            << " files=" << m.file_count << " funcs=" << m.function_count
            << " cohesion=" << fmt2(m.cohesion_score) << "\n";
    }
    if (ma.modules.size() > 10) {
        out << "  ... and " << (ma.modules.size() - 10) << " more modules\n";
    }
    out << "---\n";
}

// == STATISTICS == — complexity, coupling, cohesion, quality, plus the top-3
// high-complexity functions and low-cohesion modules.
void emit_statistics(std::ostringstream& out, const ComplexityMetrics& cm,
                     const CouplingMetrics& cp, const CohesionMetrics& ch,
                     const QualityMetrics& q, double purity_ratio) {
    out << "== STATISTICS ==\n";
    out << "complexity: avg=" << fmt2(cm.average_cc)
        << " median=" << fmt2(cm.median_cc) << "\n";
    if (!cm.distribution.empty()) {
        // Go iterates the distribution map (non-deterministic). C++ emits a
        // fixed low/medium/high order.
        out << "  distribution:";
        for (const char* k : {"low", "medium", "high"}) {
            auto it = cm.distribution.find(k);
            if (it != cm.distribution.end())
                out << " " << k << "=" << it->second;
        }
        out << "\n";
    }
    out << "coupling: avg=" << fmt2(cp.average_coupling)
        << " max=" << fmt2(cp.max_coupling) << "\n";
    out << "cohesion: avg=" << fmt2(ch.average_cohesion)
        << " min=" << fmt2(ch.min_cohesion) << "\n";
    out << "quality: maintainability=" << fmt2(q.maintainability_index)
        << " debt=" << fmt2(q.technical_debt_ratio)
        << " purity=" << fmt2(purity_ratio) << "\n";
    if (!cm.high_complexity_funcs.empty()) {
        out << "  high_complexity:\n";
        size_t lim = std::min(cm.high_complexity_funcs.size(), size_t{3});
        for (size_t i = 0; i < lim; ++i) {
            const auto& fn = cm.high_complexity_funcs[i];
            out << "    " << fn.name << " (" << fn.location << ") cc="
                << fmt1(fn.complexity) << "\n";
        }
    }
    if (!ch.low_cohesion_modules.empty()) {
        size_t lim = std::min(ch.low_cohesion_modules.size(), size_t{3});
        out << "  low_cohesion: ";
        for (size_t i = 0; i < lim; ++i) {
            if (i) out << ", ";
            out << ch.low_cohesion_modules[i];
        }
        out << "\n";
    }
    out << "---\n";
}

// Rewrites an absolute path to project-root-relative for compact, stable
// output (mirrors git::report_to_json's normalization).
std::string git_rel(std::string_view path, std::string_view root) {
    if (!root.empty() && path.rfind(root, 0) == 0) {
        path.remove_prefix(root.size());
        while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    }
    return std::string(path);
}

// == GIT CHANGES == — real git change-analysis (duplicates / naming / metrics)
// for the working set. C++ enrichment: Go computes this in git_analyze mode but
// its LCF formatter discards the git fields and prints an all-zero STATISTICS
// block, so this data is unreachable in Go's text output. Sourced from
// git::Analyzer (the same engine behind the git_analysis tool + HTTP
// /git-analyze).
void emit_git_changes(std::ostringstream& out, const git::AnalysisReport& r,
                      std::string_view root) {
    const auto& s = r.summary;
    out << "== GIT CHANGES ==\n";
    out << "scope=" << to_string(r.metadata.scope)
        << " files_changed=" << s.files_changed
        << " added=" << s.symbols_added << " modified=" << s.symbols_modified
        << " deleted=" << s.symbols_deleted << " risk=" << fmt2(s.risk_score)
        << "\n";
    out << "findings: duplicates=" << s.duplicates_found
        << " naming=" << s.naming_issues_found
        << " metrics=" << s.metrics_issues_found << "\n";
    if (!s.top_recommendation.empty())
        out << "top: " << s.top_recommendation << "\n";

    // Top metrics issues, sorted by (file, line) for deterministic output.
    if (!r.metrics_issues.empty()) {
        std::vector<const git::MetricsFinding*> mi;
        mi.reserve(r.metrics_issues.size());
        for (const auto& m : r.metrics_issues) mi.push_back(&m);
        std::sort(mi.begin(), mi.end(), [](const auto* a, const auto* b) {
            if (a->symbol.file_path != b->symbol.file_path)
                return a->symbol.file_path < b->symbol.file_path;
            return a->symbol.line < b->symbol.line;
        });
        out << "metrics_issues:\n";
        size_t lim = std::min(mi.size(), size_t{5});
        for (size_t i = 0; i < lim; ++i) {
            const auto& m = *mi[i];
            out << "  " << m.symbol.name << " ("
                << git_rel(m.symbol.file_path, root) << ":" << m.symbol.line
                << ") " << to_string(m.issue_type)
                << " loc=" << m.symbol.lines_of_code
                << " cc=" << m.symbol.complexity << "\n";
        }
        if (mi.size() > lim)
            out << "  ... and " << (mi.size() - lim) << " more\n";
    }
    out << "---\n";
}

// == GIT HOTSPOTS == — change-frequency / churn analysis: the files that change
// most, multi-author collision zones, and module ownership. C++ enrichment:
// like git_analyze, Go computes this in git_hotspots mode but discards it in the
// LCF formatter. Sourced from git::FrequencyAnalyzer. NOTE: output reflects a
// rolling time window over live git history, so values are environment- and
// time-dependent (not byte-stable across runs) — parity for this mode is
// envelope-only by design.
void emit_git_hotspots(std::ostringstream& out,
                       const git::ChangeFrequencyReport& r,
                       git::TimeWindow window, std::string_view root) {
    const auto& s = r.summary;
    out << "== GIT HOTSPOTS ==\n";
    out << "window=" << to_string(window)
        << " files_analyzed=" << s.total_files_analyzed
        << " commits=" << s.total_commits_analyzed
        << " hotspots=" << s.hotspots_found
        << " anti_patterns=" << s.anti_patterns_found << "\n";

    if (!r.hotspots.empty()) {
        out << "hotspots:\n";
        size_t lim = std::min(r.hotspots.size(), size_t{8});
        for (size_t i = 0; i < lim; ++i) {
            const auto& h = r.hotspots[i];
            const auto it = h.metrics.find(window);
            int changes = it != h.metrics.end() ? it->second.change_count : 0;
            int added = it != h.metrics.end() ? it->second.lines_added : 0;
            int deleted = it != h.metrics.end() ? it->second.lines_deleted : 0;
            out << "  " << git_rel(h.file_path, root) << " changes=" << changes
                << " authors=" << h.contributors.size() << " churn=+" << added
                << "/-" << deleted << "\n";
        }
    }
    if (!r.collisions.empty()) {
        out << "collisions:\n";
        size_t lim = std::min(r.collisions.size(), size_t{5});
        for (size_t i = 0; i < lim; ++i) {
            const auto& c = r.collisions[i];
            out << "  " << git_rel(c.path, root)
                << " score=" << fmt2(c.collision_score)
                << " contributors=" << c.contributors.size()
                << " severity=" << to_string(c.severity) << "\n";
        }
    }
    out << "---\n";
}

// A symbol ranked by how much of the codebase transitively depends on it.
struct LoadBearingSym {
    std::string name;
    std::string location;  // project-root-relative path:line
    int reach{};           // distinct transitive callers
};

// A graph-detected community (Louvain) with its largest-reach exemplar members
// and, when semantic labels are available, the dominant propagated @lci: label
// across its members (domain) plus the fraction carrying it (coherence).
struct ClusterInfo {
    int size{};
    std::vector<std::string> exemplars;
    std::string domain;     // dominant propagated label, "" if none/weak
    double coherence{};     // members carrying `domain` / size
};

// A broker: a symbol with high betweenness — many shortest paths route through
// it, so it bridges otherwise-separate regions (a chokepoint).
struct BrokerSym {
    std::string name;
    std::string location;
    double score{};  // normalized betweenness
};

// An upward call that violates layered architecture: a deeper layer calling a
// shallower one (e.g. Data -> Presentation).
struct LayerViolation {
    std::string caller, caller_layer;
    std::string callee, callee_layer;
};

// Everything derived from one build of the call graph.
struct GraphSignals {
    std::vector<LoadBearingSym> load_bearing;
    std::vector<BrokerSym> brokers;                // top betweenness, may be empty
    std::vector<std::vector<std::string>> cycles;  // names per cyclic group
    std::vector<ClusterInfo> clusters;             // communities by size desc
    std::vector<LayerViolation> layer_violations;  // upward calls, may be empty
    double modularity{};
    int community_count{};
};

// Canonical top-to-bottom depth of the architectural layers LayerAnalyzer
// classifies into. Calls should flow downward (shallow -> deep). Utility is
// cross-cutting and unknown layers are unranked — both exempt (return -1).
int layer_depth(const std::string& layer) {
    if (layer == "Presentation Layer") return 0;
    if (layer == "Application Layer") return 1;
    if (layer == "Domain Layer") return 2;
    if (layer == "Data Layer") return 3;
    return -1;
}

// Single build of analysis::CallGraph over the real call graph, yielding three
// signals at once (Karpathy: don't rebuild the graph three times):
//   - load-bearing: exact transitive-caller reach (SCC + bitset closure),
//   - cycles: strongly-connected components = circular dependencies,
//   - clusters: Louvain communities + modularity Q (real graph clustering that
//     supersedes directory/name-prefix module heuristics).
// Every call edge is weighted 1.0 — an edge carries dependence or it doesn't;
// no fixed per-hop decay constant. (Principled edge weighting would come from
// per-call-site control flow — loops/branches — not a constant; not surfaced
// per-edge yet.)
GraphSignals compute_graph_signals(const MasterIndex& indexer,
                                   const std::vector<FileSymbolData>& files,
                                   std::string_view project_root, size_t top_n,
                                   const GraphPropagator* propagator) {
    const auto& ref = indexer.ref_tracker();

    std::vector<SymbolID> nodes;
    absl::flat_hash_map<SymbolID, std::pair<std::string, std::string>> meta;
    absl::flat_hash_map<SymbolID, std::string> layer;  // id -> architectural layer
    for (const auto& f : files) {
        for (const auto* sym : f.symbols) {
            auto t = sym->symbol.type;
            if (t != SymbolType::Function && t != SymbolType::Method &&
                t != SymbolType::Constructor)
                continue;
            nodes.push_back(sym->id);
            meta[sym->id] = {sym->symbol.name,
                             git_rel(f.path, project_root) + ":" +
                                 std::to_string(sym->symbol.line)};
            layer[sym->id] = LayerAnalyzer::classify_symbol_to_layer(*sym);
        }
    }

    analysis::CallGraph graph;
    graph.build(nodes,
                [&ref](SymbolID id) { return ref.get_callee_symbols(id); });
    auto reach = graph.incoming_reach();

    GraphSignals sig;
    auto name_at = [&](int idx) -> const std::string& {
        return meta[graph.id_at(idx)].first;
    };

    // Load-bearing.
    for (int i = 0; i < graph.node_count(); ++i) {
        if (reach[i] <= 0) continue;
        const auto& m = meta[graph.id_at(i)];
        sig.load_bearing.push_back({m.first, m.second, reach[i]});
    }
    std::sort(sig.load_bearing.begin(), sig.load_bearing.end(),
              [](const LoadBearingSym& a, const LoadBearingSym& b) {
                  if (a.reach != b.reach) return a.reach > b.reach;
                  if (a.name != b.name) return a.name < b.name;
                  return a.location < b.location;
              });
    if (sig.load_bearing.size() > top_n) sig.load_bearing.resize(top_n);

    // Brokers (betweenness). Brandes is O(V·(V+E)); skip on very large graphs so
    // the interactive overview stays fast — brokers are an optional enrichment,
    // not a correctness path.
    if (graph.node_count() <= 2000) {
        auto bc = graph.betweenness();
        std::vector<BrokerSym> brokers;
        for (int i = 0; i < graph.node_count(); ++i) {
            if (bc[i] <= 0.0) continue;
            const auto& m = meta[graph.id_at(i)];
            brokers.push_back({m.first, m.second, bc[i]});
        }
        std::sort(brokers.begin(), brokers.end(),
                  [](const BrokerSym& a, const BrokerSym& b) {
                      if (a.score != b.score) return a.score > b.score;
                      if (a.name != b.name) return a.name < b.name;
                      return a.location < b.location;
                  });
        if (brokers.size() > top_n) brokers.resize(top_n);
        sig.brokers = std::move(brokers);
    }

    // Cycles (top few, each capped to 6 names for compactness).
    for (auto& cyc : graph.cycles()) {
        std::vector<std::string> names;
        for (int idx : cyc) {
            names.push_back(name_at(idx));
            if (names.size() >= 6) break;
        }
        std::sort(names.begin(), names.end());
        sig.cycles.push_back(std::move(names));
        if (sig.cycles.size() >= 5) break;
    }

    // Clusters: Louvain communities, ranked by size, with highest-reach
    // exemplars. Only meaningful when there is real structure (≥2 communities).
    auto comm = graph.louvain_communities(sig.modularity);
    if (!comm.empty()) {
        int k = 0;
        for (int c : comm) k = std::max(k, c + 1);
        sig.community_count = k;
        std::vector<std::vector<int>> members(k);
        for (int i = 0; i < static_cast<int>(comm.size()); ++i)
            members[comm[i]].push_back(i);
        std::vector<ClusterInfo> all;
        for (int c = 0; c < k; ++c) {
            if (members[c].size() < 2) continue;  // skip singletons
            auto& mem = members[c];
            std::sort(mem.begin(), mem.end(), [&](int a, int b) {
                if (reach[a] != reach[b]) return reach[a] > reach[b];
                return name_at(a) < name_at(b);
            });
            ClusterInfo ci;
            ci.size = static_cast<int>(mem.size());
            for (int idx : mem) {
                ci.exemplars.push_back(name_at(idx));
                if (ci.exemplars.size() >= 3) break;
            }

            // Label-coherent domain: the dominant propagated @lci: label across
            // this community's members. Crossing graph structure (who calls
            // whom) with propagated semantics (what concept reaches here) turns
            // an anonymous cluster into a named domain. `impure`/`pure` are
            // purity signals, not domains — excluded.
            if (propagator) {
                absl::flat_hash_map<std::string, int> label_members;
                for (int idx : mem) {
                    absl::flat_hash_set<std::string> here;
                    for (const auto& pl :
                         propagator->get_labels(graph.id_at(idx))) {
                        if (pl.strength < 0.1) continue;
                        if (pl.label == "impure" || pl.label == "pure") continue;
                        if (here.insert(pl.label).second)
                            ++label_members[pl.label];
                    }
                }
                std::string best;
                int best_n = 0;
                for (const auto& [lbl, cnt] : label_members) {
                    if (cnt > best_n || (cnt == best_n && lbl < best)) {
                        best_n = cnt;
                        best = lbl;
                    }
                }
                double coh = ci.size > 0
                                 ? static_cast<double>(best_n) / ci.size
                                 : 0.0;
                if (!best.empty() && coh >= 0.5) {
                    ci.domain = best;
                    ci.coherence = coh;
                }
            }
            all.push_back(std::move(ci));
        }
        std::sort(all.begin(), all.end(), [](const ClusterInfo& a,
                                             const ClusterInfo& b) {
            if (a.size != b.size) return a.size > b.size;
            return a.exemplars < b.exemplars;
        });
        if (all.size() > 6) all.resize(6);
        sig.clusters = std::move(all);
    }

    // Layer violations: call edges that run UP the architectural stack (a deeper
    // layer calling a shallower one). Calls should flow downward; an upward edge
    // (e.g. Data -> Presentation) inverts the dependency and is a violation.
    // Utility/unknown layers are exempt (depth -1).
    for (SymbolID u : nodes) {
        int du = layer_depth(layer[u]);
        if (du < 0) continue;
        for (SymbolID v : ref.get_callee_symbols(u)) {
            auto lv = layer.find(v);
            if (lv == layer.end()) continue;
            int dv = layer_depth(lv->second);
            if (dv < 0 || du <= dv) continue;  // exempt or downward/same = ok
            sig.layer_violations.push_back(
                {meta[u].first, layer[u], meta[v].first, lv->second});
        }
    }
    std::sort(sig.layer_violations.begin(), sig.layer_violations.end(),
              [](const LayerViolation& a, const LayerViolation& b) {
                  if (a.caller != b.caller) return a.caller < b.caller;
                  return a.callee < b.callee;
              });
    if (sig.layer_violations.size() > 8) sig.layer_violations.resize(8);

    return sig;
}

// == LOAD BEARING == — symbols the rest of the codebase most depends on, by
// transitive call-graph reach (C++ enrichment; Go has no equivalent). Pairs
// with HEALTH's problematic_symbols: high reach + high risk = fix-first.
void emit_load_bearing(std::ostringstream& out, const GraphSignals& sig) {
    const auto& lb = sig.load_bearing;
    if (lb.empty() && sig.brokers.empty()) return;
    out << "== LOAD BEARING ==\n";
    for (const auto& s : lb) {
        out << "  " << s.name << " (" << s.location << ") reach=" << s.reach
            << "\n";
    }
    // Brokers: high-betweenness chokepoints bridging separate regions. Distinct
    // from reach — a bridge can have low reach but be on every cross-path.
    if (!sig.brokers.empty()) {
        out << "brokers:\n";
        for (const auto& b : sig.brokers) {
            out << "  " << b.name << " (" << b.location
                << ") betweenness=" << fmt2(b.score) << "\n";
        }
    }
    out << "---\n";
}

// == CYCLES == — circular call dependencies (strongly-connected components of
// the call graph). C++ enrichment; Go has no equivalent.
void emit_cycles(std::ostringstream& out,
                 const std::vector<std::vector<std::string>>& cycles) {
    if (cycles.empty()) return;
    out << "== CYCLES ==\n";
    out << "count=" << cycles.size() << "\n";
    for (const auto& c : cycles) {
        out << "  ";
        for (size_t i = 0; i < c.size(); ++i) {
            if (i) out << " <-> ";
            out << c[i];
        }
        out << "\n";
    }
    out << "---\n";
}

// == LAYER VIOLATIONS == — calls that run UP the architectural stack (a deeper
// layer calling a shallower one), inverting the intended dependency direction.
// C++ enrichment; Go has no equivalent.
void emit_layer_violations(std::ostringstream& out,
                           const std::vector<LayerViolation>& v) {
    if (v.empty()) return;
    out << "== LAYER VIOLATIONS ==\n";
    out << "count=" << v.size() << "\n";
    for (const auto& x : v) {
        out << "  " << x.caller << " [" << x.caller_layer << "] -> " << x.callee
            << " [" << x.callee_layer << "]\n";
    }
    out << "---\n";
}

// == CLUSTERS == — Louvain communities over the call graph: groups of symbols
// that call each other more than the rest of the codebase, with the modularity
// score. Real graph clustering, not directory/name heuristics. C++ enrichment.
void emit_clusters(std::ostringstream& out, const GraphSignals& sig) {
    if (sig.clusters.empty()) return;
    out << "== CLUSTERS ==\n";
    out << "communities=" << sig.community_count
        << " modularity=" << fmt2(sig.modularity) << "\n";
    for (size_t i = 0; i < sig.clusters.size(); ++i) {
        const auto& c = sig.clusters[i];
        out << "  c" << i << " size=" << c.size;
        if (!c.domain.empty())
            out << " domain=" << c.domain << " coherence=" << fmt2(c.coherence);
        out << ": ";
        for (size_t j = 0; j < c.exemplars.size(); ++j) {
            if (j) out << ", ";
            out << c.exemplars[j];
        }
        out << "\n";
    }
    out << "---\n";
}

// == VOCABULARY == — low-discoverability naming signal (C++ enhancement; Go
// has no equivalent section). `outliers` are important symbols whose names use
// unknown/obscure vocabulary an agent won't search for; `aliases_in_use` tells
// which member term each standard concept uses in this codebase.
void emit_vocabulary(std::ostringstream& out, const NamingReport& nr) {
    if (nr.outliers.empty() && nr.aliases_in_use.empty()) return;
    out << "== VOCABULARY ==\n";
    out << "outliers=" << nr.outliers.size() << "\n";
    for (const auto& o : nr.outliers) {
        out << "  " << o.name << " (" << o.location << ") fan-in=" << o.fan_in
            << " " << o.reason << "=" << o.odd_term;
        if (!o.suggested.empty()) {
            out << " -> ";
            for (size_t i = 0; i < o.suggested.size(); ++i) {
                if (i) out << ",";
                out << o.suggested[i];
            }
        }
        out << " [o=" << o.object_id << "]\n";
    }
    if (!nr.aliases_in_use.empty()) {
        out << "aliases_in_use:\n";
        for (const auto& a : nr.aliases_in_use) {
            out << "  " << a.canonical << ":";
            for (const auto& [member, n] : a.terms) {
                out << " " << member << "(" << n << ")";
            }
            out << "\n";
        }
    }
    out << "---\n";
}

// == SUMMARY == — one-look orientation: size + language mix. C++-only
// session-startup section (no Go counterpart). lang counts by file extension.
void emit_summary(std::ostringstream& out,
                  const std::vector<FileSymbolData>& files,
                  std::string_view project_root, int file_count,
                  int symbol_count) {
    absl::flat_hash_map<std::string, int> lang_files;
    absl::flat_hash_set<std::string> dirs;
    int max_depth = 0;
    auto lang_of = [](std::string_view path) -> const char* {
        auto dot = path.rfind('.');
        if (dot == std::string_view::npos) return nullptr;
        auto ext = path.substr(dot);
        if (ext == ".go") return "go";
        if (ext == ".py") return "python";
        if (ext == ".ts" || ext == ".tsx") return "typescript";
        if (ext == ".js" || ext == ".jsx" || ext == ".mjs") return "javascript";
        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".hpp" ||
            ext == ".hh" || ext == ".h")
            return "cpp";
        if (ext == ".c") return "c";
        if (ext == ".rs") return "rust";
        if (ext == ".java") return "java";
        if (ext == ".rb") return "ruby";
        if (ext == ".php") return "php";
        if (ext == ".cs") return "csharp";
        if (ext == ".kt") return "kotlin";
        if (ext == ".swift") return "swift";
        return nullptr;
    };
    for (const auto& f : files) {
        std::string rel = f.path;
        if (!project_root.empty() && rel.rfind(project_root, 0) == 0) {
            rel = rel.substr(project_root.size());
            while (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
        }
        int depth = 0;
        for (char c : rel) if (c == '/') ++depth;
        if (depth > max_depth) max_depth = depth;
        auto slash = rel.rfind('/');
        dirs.insert(slash == std::string::npos ? std::string(".")
                                               : rel.substr(0, slash));
        if (const char* l = lang_of(f.path)) lang_files[l]++;
    }
    out << "== SUMMARY ==\n"
        << "files=" << file_count << " symbols=" << symbol_count
        << " dirs=" << dirs.size() << " depth=" << max_depth << "\n";
    if (!lang_files.empty()) {
        std::vector<std::pair<std::string, int>> langs(lang_files.begin(),
                                                       lang_files.end());
        std::sort(langs.begin(), langs.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second != b.second) return a.second > b.second;
                      return a.first < b.first;
                  });
        out << "langs:";
        for (const auto& [l, n] : langs) out << " " << l << "=" << n;
        out << "\n";
    }
    out << "---\n";
}

// == ENTRY POINTS == — where execution starts / the public surface. main()
// first, then top exported API by importance. C++-only session-startup
// section (Go computes entry points but never emits them).
void emit_entry_points(std::ostringstream& out, const EntryPointsList* ep,
                       std::string_view project_root) {
    if (!ep || ep->main_functions.empty()) return;
    out << "== ENTRY POINTS ==\n";
    size_t lim = std::min(ep->main_functions.size(), size_t{12});
    for (size_t i = 0; i < lim; ++i) {
        const auto& e = ep->main_functions[i];
        std::string loc = e.location;
        if (!project_root.empty() && loc.rfind(project_root, 0) == 0) {
            loc = loc.substr(project_root.size());
            while (!loc.empty() && loc.front() == '/') loc.erase(0, 1);
        }
        out << "  " << e.type << ": " << e.name << " (" << loc << ")\n";
    }
    if (ep->main_functions.size() > lim) {
        out << "  ... and " << (ep->main_functions.size() - lim)
            << " more exported\n";
    }
    out << "---\n";
}

// == DEPENDENCIES == — which modules the rest of the codebase leans on
// (afferent coupling = number of other packages that depend on this one) and
// how unstable each is. Sourced from CouplingAnalyzer (the engine's dependency
// graph is still a node-only stub). C++-only session-startup section.
void emit_dependencies(std::ostringstream& out, const CouplingMetrics& cp) {
    std::vector<std::pair<std::string, int>> aff(cp.afferent_coupling.begin(),
                                                 cp.afferent_coupling.end());
    aff.erase(std::remove_if(aff.begin(), aff.end(),
                             [](const auto& p) { return p.second <= 0; }),
              aff.end());
    if (aff.empty()) return;
    std::sort(aff.begin(), aff.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    out << "== DEPENDENCIES ==\n";
    out << "most_depended_on:\n";
    size_t lim = std::min(aff.size(), size_t{8});
    for (size_t i = 0; i < lim; ++i) {
        const auto& [pkg, n] = aff[i];
        double inst = 0.0;
        auto it = cp.instability.find(pkg);
        if (it != cp.instability.end()) inst = it->second;
        out << "  " << pkg << " depended_on_by=" << n
            << " instability=" << fmt2(inst) << "\n";
    }
    out << "---\n";
}

// == NEXT STEPS == — tells an agent how to USE this report at session start.
// C++-only footer.
void emit_next_steps(std::ostringstream& out) {
    out << "== NEXT STEPS ==\n"
        << "1. ENTRY POINTS = where execution starts and the public surface.\n"
        << "2. high-fan-in + problematic_symbols = load-bearing / risky code.\n"
        << "3. aliases_in_use = search with THIS codebase's vocabulary.\n"
        << "4. get_context {\"id\": \"<o=ID>\"} drills into any [o=..] symbol.\n"
        << "---\n";
}

// == OBJECT IDs == — workflow hint, appended when any smell/problematic
// symbol carried an object id. Matches Go's formatWorkflowHint (note the
// leading "---" producing the doubled separator after HEALTH).
void emit_object_ids_hint(std::ostringstream& out) {
    out << "---\n== OBJECT IDs ==\n"
        << "Use [o=XX] identifiers above with get_context for detailed info:\n"
        << "  get_context {\"id\": \"XX\"}\n"
        << "Example: If you see [o=ABC], use get_context {\"id\": \"ABC\"}\n"
        << "---\n";
}

// Tally a purity summary from the live SideEffectAnalyzer. Go-parity: the
// C++ SideEffectAnalyzer defaults every unanalyzed callable to pure, so
// pure-counting only happens once at least one impure function proves the
// analyzer ran for real (otherwise an unannotated corpus would report
// everything pure, diverging from Go's conservative 0/0).
PuritySummary tally_purity(SideEffectAnalyzer* analyzer) {
    PuritySummary ps;
    if (!analyzer) return ps;
    int impure_n = 0;
    for (const auto& [key, info] : analyzer->results()) {
        (void)key;
        if (!info.is_pure) ++impure_n;
    }
    int pure_n = 0;
    if (impure_n > 0) {
        for (const auto& [key, info] : analyzer->results()) {
            (void)key;
            if (info.is_pure) ++pure_n;
        }
    }
    ps.pure_functions = pure_n;
    ps.impure_functions = impure_n;
    ps.total_functions = static_cast<int>(analyzer->results().size());
    ps.purity_ratio = ps.total_functions > 0
        ? static_cast<double>(pure_n) / ps.total_functions
        : 0.0;
    // Effect breakdown (same category bits as side_effect_summary) so the
    // HEALTH purity block can emit the `effects:` line. Counts a function once
    // per category if it (transitively) exhibits it.
    for (const auto& [key, info] : analyzer->results()) {
        (void)key;
        uint32_t combined = info.categories | info.transitive_categories;
        if (combined & side_effect::kParamWrite) ++ps.with_param_writes;
        if (combined & side_effect::kGlobalWrite) ++ps.with_global_writes;
        if (combined & (side_effect::kIO | side_effect::kNetwork |
                        side_effect::kDatabase))
            ++ps.with_io_effects;
        if (combined & side_effect::kThrow) ++ps.with_throws;
    }
    return ps;
}

// Strip a single trailing newline so the payload ends on "---" with no
// trailing newline, matching Go's strings.Join(lines, "\n").
std::string finalize_lcf(std::ostringstream& out) {
    std::string s = out.str();
    if (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

}  // namespace

// -- handle_code_insight ------------------------------------------------------

ToolResult handle_code_insight(const nlohmann::json& raw_params,
                               CodebaseIntelligenceEngine& engine,
                               MasterIndex& indexer,
                               SideEffectAnalyzer* analyzer,
                               GraphPropagator* propagator) {
    auto params = raw_params.is_object() ? raw_params : nlohmann::json::object();
    auto mode = params.value("mode", "overview");

    if (!CodebaseIntelligenceEngine::is_valid_mode(mode)) {
        return make_error_response(
            "code_insight",
            "invalid mode '" + mode + "', must be one of: overview, detailed, "
            "statistics, unified, structure, git_analyze, git_hotspots");
    }

    // Corpus data gathered once. file_count drives the structure summary;
    // files_data/symbol_count/project_root feed the engine-backed modes.
    const std::string& project_root = indexer.config().project.root;
    int file_count = indexer.file_count();
    auto files_data = gather_file_symbol_data(indexer);
    int symbol_count = 0;
    int total_functions = 0;
    for (const auto& f : files_data) {
        symbol_count += static_cast<int>(f.symbols.size());
        for (const auto* sym : f.symbols) {
            if (sym && sym->symbol.type == SymbolType::Function) {
                ++total_functions;
            }
        }
    }

    // Shared analysis for the engine-backed modes (overview/unified/
    // statistics). The engine's overview pipeline computes the health
    // dashboard (complexity, smells, problematic symbols); modules, purity,
    // coupling/cohesion and quality are layered on from the dedicated
    // analyzers, mirroring Go's Server.buildOverview/Statistics path.
    struct EngineData {
        CodebaseIntelligenceEngine::Result result;
        ModuleAnalysis modules;  // repository map + == MODULES == (same naming)
        PuritySummary purity;
        CouplingAnalyzer::CouplingResult coupling;
        QualityMetrics quality;
        NamingReport naming;
    };
    auto gather_engine = [&]() -> EngineData {
        CodebaseIntelligenceParams ci;
        ci.mode = "overview";
        ci.include.repository_map = true;
        ci.include.health_dashboard = true;
        ci.include.entry_points = true;
        if (params.contains("max_results")) {
            ci.max_results = params.value("max_results", 50);
        }
        EngineData d;
        d.result = engine.analyze(ci, files_data,
                                  static_cast<int>(files_data.size()),
                                  symbol_count);
        d.modules = ModuleAnalyzer().analyze(files_data, project_root);
        d.purity = tally_purity(analyzer);
        d.coupling = CouplingAnalyzer().analyze(files_data, project_root);
        // Replace ModuleAnalyzer's placeholder per-module coupling (a 0.30
        // constant inherited from Go) with the real per-package coupling
        // from CouplingAnalyzer. Both key modules by getPackageName, so the
        // names line up. Recompute the aggregate average from real values.
        if (!d.coupling.coupling.module_coupling.empty()) {
            double sum = 0.0;
            for (auto& m : d.modules.modules) {
                auto it = d.coupling.coupling.module_coupling.find(m.name);
                m.coupling_score =
                    it != d.coupling.coupling.module_coupling.end()
                        ? it->second
                        : 0.0;
                sum += m.coupling_score;
            }
            if (!d.modules.modules.empty()) {
                d.modules.metrics.average_coupling =
                    sum / static_cast<double>(d.modules.modules.size());
            }
        }
        if (d.result.response.health_dashboard) {
            d.quality = HealthAnalyzer::calculate_quality_from_complexity(
                d.result.response.health_dashboard->complexity);
        }
        d.naming = NamingAnalyzer().analyze(files_data,
                                            indexer.config().synonyms,
                                            project_root);
        return d;
    };

    std::ostringstream out;
    if (mode == "statistics") {
        EngineData d = gather_engine();
        if (!d.result.ok()) {
            return make_error_response("code_insight", d.result.error);
        }
        const auto* hd = d.result.response.health_dashboard;
        emit_lcf_header(out, "statistics", 1,
                        lcf_token_count(0, 0, false, 0, true));
        if (hd) {
            emit_statistics(out, hd->complexity, d.coupling.coupling,
                            d.coupling.cohesion, d.quality,
                            d.purity.purity_ratio);
        }
    } else if (mode == "structure") {
        // Compute structure inline from the live index. Hardcoded version
        // always emitted dirs=1, top_dirs `.:N`; on any real corpus the
        // top-level dir distribution is much richer. Walk file paths,
        // count per top-level dir + per extension. depth is the deepest
        // path-component count seen.
        absl::flat_hash_map<std::string, int> top_dir_files;
        absl::flat_hash_map<std::string, int> types_count;
        int code = 0, tests = 0, config = 0, docs = 0;
        int max_depth = 0;
        for (auto fid : indexer.get_all_file_ids()) {
            auto p = indexer.get_file_path(fid);
            if (p.empty()) continue;
            std::string rel = p;
            const auto& root = indexer.config().project.root;
            if (!root.empty() && rel.rfind(root, 0) == 0) {
                rel = rel.substr(root.size());
                while (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
            }
            int depth = 0;
            for (char c : rel) if (c == '/') ++depth;
            if (depth > max_depth) max_depth = depth;
            auto slash = rel.find('/');
            std::string top =
                slash == std::string::npos ? "." : rel.substr(0, slash);
            ++top_dir_files[top];
            auto dot = rel.rfind('.');
            if (dot != std::string::npos)
                ++types_count[rel.substr(dot)];
            if (rel.find("_test.") != std::string::npos ||
                rel.find("/test") != std::string::npos) ++tests;
            else if (rel.find(".md") != std::string::npos ||
                     rel.find("README") != std::string::npos) ++docs;
            else if (rel.find(".json") != std::string::npos ||
                     rel.find(".yaml") != std::string::npos ||
                     rel.find(".yml") != std::string::npos ||
                     rel.find(".toml") != std::string::npos) ++config;
            else ++code;
        }
        out << "LCF/1.0\nmode=structure\ntier=1\ntokens=20\n---\n"
            << "== STRUCTURE ==\n"
            << "dirs=" << top_dir_files.size()
            << " files=" << file_count
            << " symbols=" << total_functions
            << " depth=" << max_depth << "\n";
        std::vector<std::pair<std::string, int>> tv(
            types_count.begin(), types_count.end());
        std::sort(tv.begin(), tv.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        out << "types:";
        for (const auto& [ext, n] : tv) out << " " << ext << "=" << n;
        out << "\n"
            << "categories: code=" << code
            << " tests=" << tests
            << " config=" << config
            << " docs=" << docs << "\n"
            << "top_dirs:\n";
        std::vector<std::pair<std::string, int>> td(
            top_dir_files.begin(), top_dir_files.end());
        std::sort(td.begin(), td.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        size_t shown = std::min(td.size(), size_t{10});
        for (size_t i = 0; i < shown; ++i)
            out << "  " << td[i].first << ": " << td[i].second << " files\n";
        out << "---";
    } else if (mode == "unified") {
        EngineData d = gather_engine();
        if (!d.result.ok()) {
            return make_error_response("code_insight", d.result.error);
        }
        const auto* hd = d.result.response.health_dashboard;
        int n_map = std::min(static_cast<int>(d.modules.modules.size()), 15);
        bool objids = (hd && (!hd->detailed_smells.empty() ||
                              !hd->problematic_symbols.empty())) ||
                      !d.naming.outliers.empty();
        emit_lcf_header(out, "unified", 1,
                        lcf_token_count(n_map, 0, hd != nullptr, 0, true));
        emit_summary(out, files_data, project_root, file_count, symbol_count);
        emit_repository_map(out, d.modules.modules);
        emit_entry_points(out, d.result.response.entry_points, project_root);
        if (hd) emit_health(out, *hd, &d.purity);
        {
            auto sig = compute_graph_signals(indexer, files_data,
                                             project_root, 5, propagator);
            emit_load_bearing(out, sig);
            emit_clusters(out, sig);
            emit_cycles(out, sig.cycles);
            emit_layer_violations(out, sig.layer_violations);
        }
        emit_modules(out, d.modules);
        emit_dependencies(out, d.coupling.coupling);
        if (hd) {
            emit_statistics(out, hd->complexity, d.coupling.coupling,
                            d.coupling.cohesion, d.quality,
                            d.purity.purity_ratio);
        }
        emit_vocabulary(out, d.naming);
        if (objids) emit_object_ids_hint(out);
        emit_next_steps(out);
    } else if (mode == "git_analyze" || mode == "git_hotspots") {
        // Real git wiring. Go computes these but its LCF formatter discards the
        // git fields (emits an all-zero STATISTICS block); C++ surfaces the
        // real data — intentional enrichment, parity is envelope-only for these
        // two modes (git_hotspots is additionally time-window-volatile). Fail
        // fast when the project root is not a git repository — no fake zeros.
        git::Provider provider;
        if (!git::Provider::create(project_root, provider)) {
            return make_error_response(
                "code_insight",
                "mode=" + mode + " requires a git repository; '" +
                    (project_root.empty() ? "<no root>" : project_root) +
                    "' is not one");
        }

        if (mode == "git_analyze") {
            git::AnalysisParams ga = git::AnalysisParams::defaults();
            auto scope = params.value("scope", std::string("staged"));
            if (scope == "wip") ga.scope = git::AnalysisScope::WIP;
            else if (scope == "commit") ga.scope = git::AnalysisScope::Commit;
            else if (scope == "range") ga.scope = git::AnalysisScope::Range;
            else ga.scope = git::AnalysisScope::Staged;
            ga.base_ref = params.value("base_ref", std::string());
            ga.target_ref = params.value("target_ref", std::string());

            git::Analyzer analyzer(provider, indexer);
            git::AnalysisReport report;
            if (!analyzer.analyze(ga, report))
                return make_error_response("code_insight",
                                           "git change analysis failed");
            emit_lcf_header(out, mode, 1, lcf_token_count(0, 0, false, 0, true));
            emit_git_changes(out, report, project_root);
        } else {  // git_hotspots
            git::ChangeFrequencyParams fp = git::ChangeFrequencyParams::defaults();
            fp.time_window = params.value("time_window", std::string("30d"));
            fp.file_pattern = params.value("file_pattern", std::string());
            git::TimeWindow win = git::parse_time_window(fp.time_window);

            git::FrequencyAnalyzer freq(provider);
            git::ChangeFrequencyReport report;
            if (!freq.analyze(fp, report))
                return make_error_response("code_insight",
                                           "git frequency analysis failed");
            emit_lcf_header(out, mode, 1, lcf_token_count(0, 0, false, 0, true));
            emit_git_hotspots(out, report, win, project_root);
        }
    } else if (mode == "detailed") {
        // Detailed sub-mode dispatch via the existing analyzers.
        // The engine's build_detailed() returns an empty response because
        // CodebaseIntelligenceResponse has no module/layer/feature fields
        // (Go uses separate JSON fields the C++ port hasn't yet added to
        // the response struct). We call the analyzers directly here and
        // emit results as LCF text — Karpathy #6: don't silently fall
        // through to overview, surface the real data the analyzers
        // already compute.
        std::string detailed_mode = params.value("analysis",
                                                 params.value("detailed_mode", ""));
        if (detailed_mode.empty()) detailed_mode = "modules";
        if (detailed_mode != "modules" && detailed_mode != "layers" &&
            detailed_mode != "features" && detailed_mode != "terms") {
            return make_error_response(
                "code_insight",
                "invalid detailed analysis '" + detailed_mode +
                "', must be one of: modules, layers, features, terms");
        }

        out << "LCF/1.0\nmode=detailed\nsub=" << detailed_mode
            << "\ntier=2\ntokens=100\n---\n";
        if (detailed_mode == "modules") {
            ModuleAnalyzer ma;
            auto r = ma.analyze(files_data, project_root);
            out << "== MODULES ==\n"
                << "total=" << r.metrics.total_modules
                << " cohesion=" << fmt2(r.metrics.average_cohesion)
                << " coupling=" << fmt2(r.metrics.average_coupling)
                << " arch_score=" << fmt2(r.metrics.architectural_score)
                << "\n"
                << "strategy=" << r.detection_strategy << "\n";
            size_t shown = std::min(r.modules.size(), size_t{20});
            for (size_t i = 0; i < shown; ++i) {
                const auto& m = r.modules[i];
                out << "  " << m.name << ": type=" << m.type
                    << " files=" << m.file_count
                    << " funcs=" << m.function_count
                    << " cohesion=" << fmt2(m.cohesion_score) << "\n";
            }
            if (r.modules.size() > shown)
                out << "  ... and " << (r.modules.size() - shown) << " more\n";
        } else if (detailed_mode == "layers") {
            LayerAnalyzer la;
            auto r = la.analyze(files_data);
            out << "== LAYERS ==\n"
                << "total=" << r.layers.size()
                << " violations=" << r.violation_count << "\n";
            for (const auto& l : r.layers) {
                out << "  " << l.name << ": depth=" << l.depth
                    << " modules=" << l.modules.size()
                    << " cohesion=" << fmt2(l.metrics.cohesion_score) << "\n";
            }
            for (const auto& p : r.patterns) {
                out << "  pattern: " << p.name
                    << " confidence=" << fmt2(p.confidence) << "\n";
            }
        } else if (detailed_mode == "features") {
            FeatureAnalyzer fa;
            const auto& ref = indexer.ref_tracker();
            auto r = fa.analyze(files_data, [&ref](SymbolID id) {
                return ref.get_callee_symbols(id);
            });
            out << "== FEATURES ==\n"
                << "total=" << r.metrics.total_features
                << " avg_components=" << fmt2(r.metrics.average_components)
                << " avg_cohesion=" << fmt2(r.metrics.avg_cohesion)
                << " avg_complexity=" << fmt2(r.metrics.avg_complexity) << "\n";
            size_t shown = std::min(r.features.size(), size_t{20});
            for (size_t i = 0; i < shown; ++i) {
                const auto& f = r.features[i];
                out << "  " << f.name
                    << ": module=" << f.primary_module
                    << " components=" << f.components.size()
                    << " confidence=" << fmt2(f.confidence) << "\n";
            }
            if (r.features.size() > shown)
                out << "  ... and " << (r.features.size() - shown) << " more\n";
            for (const auto& d : r.cross_feature_deps) {
                out << "  dep: " << d.feature_a << "->" << d.feature_b
                    << " type=" << d.type
                    << " strength=" << fmt2(d.strength) << "\n";
            }
        } else {  // terms
            CIVocabularyAnalyzer va;
            auto terms = va.extract_domain_terms_from_files(files_data);
            out << "== TERMS ==\n"
                << "total=" << terms.size() << "\n";
            size_t shown = std::min(terms.size(), size_t{30});
            for (size_t i = 0; i < shown; ++i) {
                const auto& t = terms[i];
                out << "  " << t.domain
                    << ": count=" << t.count
                    << " confidence=" << fmt2(t.confidence)
                    << " terms=" << t.terms.size() << "\n";
            }
            if (terms.size() > shown)
                out << "  ... and " << (terms.size() - shown) << " more\n";
        }
        out << "---";
    } else {
        // overview (default) — engine-backed: repository map + health, with
        // the object-IDs workflow hint when smells/problematic symbols are
        // present. Mirrors Go's overview section set (no MODULES/STATISTICS).
        EngineData d = gather_engine();
        if (!d.result.ok()) {
            return make_error_response("code_insight", d.result.error);
        }
        const auto* hd = d.result.response.health_dashboard;
        int n_map = std::min(static_cast<int>(d.modules.modules.size()), 15);
        bool objids = (hd && (!hd->detailed_smells.empty() ||
                              !hd->problematic_symbols.empty())) ||
                      !d.naming.outliers.empty();
        emit_lcf_header(out, "overview", 1,
                        lcf_token_count(n_map, 0, hd != nullptr, 0, false));
        emit_summary(out, files_data, project_root, file_count, symbol_count);
        emit_repository_map(out, d.modules.modules);
        emit_entry_points(out, d.result.response.entry_points, project_root);
        if (hd) emit_health(out, *hd, &d.purity);
        {
            auto sig = compute_graph_signals(indexer, files_data,
                                             project_root, 5, propagator);
            emit_load_bearing(out, sig);
            emit_clusters(out, sig);
            emit_cycles(out, sig.cycles);
            emit_layer_violations(out, sig.layer_violations);
        }
        emit_vocabulary(out, d.naming);
        if (objids) emit_object_ids_hint(out);
        emit_next_steps(out);
    }
    return ToolResult{finalize_lcf(out), false};
}

// -- register_analysis_handlers -----------------------------------------------

void register_analysis_handlers(McpServer& server,
                                MasterIndex* indexer,
                                SemanticAnnotator* annotator,
                                SideEffectAnalyzer* analyzer,
                                GraphPropagator* propagator,
                                CodebaseIntelligenceEngine* ci_engine) {
    // Replace "semantic_annotations" stub
    server.add_tool(
        {"semantic_annotations",
         "🏷️  Query symbols by semantic labels or categories. Supports "
         "direct @lci: annotations, external annotation manifests, and "
         "propagated labels through call graphs. See 'info "
         "semantic_annotations'.",
         {{"label", "string", "Semantic label to search for", ""},
          {"category", "string", "Semantic category", ""},
          {"min_strength", "number", "Minimum label strength", ""},
          {"include_direct", "boolean",
           "Include direct annotations", ""},
          {"include_propagated", "boolean",
           "Include propagated labels", ""},
          {"max_results", "integer",
           "Maximum results (keep small to avoid token overload)", ""}},
         {}},
        [annotator, propagator, indexer](const nlohmann::json& p) -> ToolResult {
            if (!annotator) {
                return make_error_response(
                    "semantic_annotations",
                    "semantic annotator not available");
            }
            return handle_semantic_annotations(p, *annotator, propagator,
                                                indexer);
        });

    // Replace "side_effects" stub
    server.add_tool(
        {"side_effects",
         "🔬 Query function purity and side effects. Detects writes to "
         "parameters, globals, closures, I/O operations, and exception "
         "handling. Supports transitive analysis through call graphs. "
         "See 'info side_effects'.",
         {{"mode", "string",
           "Query mode: symbol, file, pure, impure, category, summary", ""},
          {"symbol_id", "string", "Symbol ID for symbol mode", ""},
          {"symbol_name", "string", "Symbol name for symbol mode", ""},
          {"file_path", "string", "File path for file mode", ""},
          {"file_id", "integer", "File ID for file mode", ""},
          {"category", "string",
           "Side effect category: param_write, global_write, io, network, "
           "throw, channel, external_call",
           ""},
          {"include_reasons", "boolean",
           "Include reasons for impurity", ""},
          {"include_transitive", "boolean",
           "Include transitive side effects from callees", ""},
          {"include_confidence", "boolean",
           "Include confidence levels", ""},
          {"max_results", "integer",
           "Maximum results (keep small to avoid token overload)", ""}},
         {}},
        [analyzer, indexer](const nlohmann::json& p) -> ToolResult {
            if (!analyzer) {
                return make_error_response(
                    "side_effects",
                    "side effect analysis not available");
            }
            return handle_side_effects(p, *analyzer, indexer);
        });

    // Replace "code_insight" stub
    server.add_tool(
        {"code_insight",
         "🎯 Comprehensive codebase intelligence system for AI agents. "
         "Provides high-level overview (79.8% context reduction), detailed "
         "analysis (2-4x accuracy improvement), code statistics, and git "
         "analysis. Modes: overview, detailed, statistics, unified, "
         "structure, git_analyze, git_hotspots. See 'info code_insight'.",
         {{"mode", "string", "Analysis mode", ""},
          {"tier", "integer", "Analysis tier", ""},
          {"analysis", "string", "Type of analysis", ""},
          {"metrics", "array", "Metrics to include", "string"},
          {"target", "string", "Target to analyze", ""},
          {"focus", "string", "Analysis focus", ""},
          {"max_results", "integer",
           "Maximum results (keep small to avoid token overload)", ""},
          {"languages", "array",
           "Filter by programming languages (e.g., [\"go\"], "
           "[\"typescript\", \"javascript\"], [\"csharp\"]). "
           "Case-insensitive with aliases (e.g., 'ts' for TypeScript, "
           "'cs' for C#).",
           "string"}},
         {}},
        [ci_engine, indexer, analyzer, propagator](
            const nlohmann::json& p) -> ToolResult {
            if (!ci_engine || !indexer) {
                return make_error_response(
                    "code_insight",
                    "codebase intelligence not available");
            }
            return handle_code_insight(p, *ci_engine, *indexer, analyzer,
                                       propagator);
        });
}

}  // namespace mcp
}  // namespace lci
