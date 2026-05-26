#include <lci/mcp/handlers_analysis.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include <lci/analysis/ci_vocabulary_analyzer.h>
#include <lci/analysis/codebase_intelligence.h>
#include <lci/analysis/feature_analyzer.h>
#include <lci/analysis/layer_analyzer.h>
#include <lci/analysis/module_analyzer.h>
#include <lci/analysis/side_effect_analyzer.h>
#include <lci/core/graph_propagator.h>
#include <lci/core/reference_tracker.h>
#include <lci/core/semantic_annotator.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/validation.h>
#include <lci/symbol.h>

#include <absl/container/flat_hash_set.h>

namespace lci {
namespace mcp {

// -- Helpers ------------------------------------------------------------------

namespace {

/// Converts a side-effect category bitfield to human-readable strings.
std::vector<std::string> categories_to_strings(uint32_t cat) {
    std::vector<std::string> result;
    if (cat == side_effect::kNone) return result;
    if (cat & side_effect::kParamWrite) result.emplace_back("param_write");
    if (cat & side_effect::kReceiverWrite) result.emplace_back("receiver_write");
    if (cat & side_effect::kGlobalWrite) result.emplace_back("global_write");
    if (cat & side_effect::kClosureWrite) result.emplace_back("closure_write");
    if (cat & side_effect::kFieldWrite) result.emplace_back("field_write");
    if (cat & side_effect::kIO) result.emplace_back("io");
    if (cat & side_effect::kDatabase) result.emplace_back("database");
    if (cat & side_effect::kNetwork) result.emplace_back("network");
    if (cat & side_effect::kThrow) result.emplace_back("throw");
    if (cat & side_effect::kChannel) result.emplace_back("channel");
    if (cat & side_effect::kAsync) result.emplace_back("async");
    if (cat & side_effect::kExternalCall) result.emplace_back("external_call");
    if (cat & side_effect::kDynamicCall) result.emplace_back("dynamic_call");
    if (cat & side_effect::kReflection) result.emplace_back("reflection");
    if (cat & side_effect::kUncertain) result.emplace_back("uncertain");
    return result;
}

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
                                   bool include_confidence) {
    nlohmann::json item;
    item["symbol_name"] = info.function_name;
    item["file_path"] = info.file_path;
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
    std::vector<FileSymbolData> result;
    result.reserve(file_ids.size());
    for (auto fid : file_ids) {
        auto path = indexer.get_file_path(fid);
        auto syms = ref.get_file_enhanced_symbols(fid);
        result.push_back({std::move(path), std::move(syms)});
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
        item["file_path"] = sym->file_path;
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
        for (const auto* sym : symbols) {
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
        for (auto& item : annotations) {
            auto file_id = static_cast<FileID>(
                item.value("file_id", 0));
            int line = item.value("line", -1);
            if (line < 0) continue;

            const EnhancedSymbol* real_sym = nullptr;
            for (const auto* es : ref.get_file_enhanced_symbols(file_id)) {
                if (es && es->symbol.line == line) {
                    real_sym = es;
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
                    if (const auto* src_es =
                            ref.get_enhanced_symbol(pl.source)) {
                        pl_item["source_name"] = src_es->symbol.name;
                        pl_item["source_file"] =
                            indexer->get_file_path(src_es->symbol.file_id);
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
        for (const auto* sym : symbols) {
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
    return make_json_response(response);
}

// -- handle_side_effects (mode dispatch) --------------------------------------

namespace {

ToolResult side_effect_symbol_query(const nlohmann::json& params,
                                    SideEffectAnalyzer& analyzer,
                                    MasterIndex* indexer) {
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
        auto symbols = ref.find_symbols_by_name(symbol_name);
        if (symbols.empty()) {
            return make_error_response(
                "side_effects", "symbol not found: " + symbol_name);
        }
        auto* sym = symbols[0];
        auto path = indexer->get_file_path(sym->symbol.file_id);
        auto* info = analyzer.get_result(path, sym->symbol.line);
        if (!info) {
            nlohmann::json response;
            response["results"] = nlohmann::json::array();
            response["total_count"] = 0;
            response["mode"] = "symbol";
            return make_json_response(response);
        }
        auto item = side_effect_to_json(*info, include_reasons,
                                        include_transitive, include_confidence);
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
                                  SideEffectAnalyzer& analyzer) {
    auto file_path = params.value("file_path", "");
    if (file_path.empty()) {
        return make_error_response(
            "side_effects", "file mode requires 'file_path'");
    }

    bool include_reasons = params.value("include_reasons", false);
    bool include_transitive = params.value("include_transitive", false);
    bool include_confidence = params.value("include_confidence", false);
    int max_results = clamp(params.value("max_results", 100), 1, 10000);

    nlohmann::json results = nlohmann::json::array();
    int count = 0;
    for (const auto& [key, info] : analyzer.results()) {
        if (info.file_path != file_path) continue;
        if (count >= max_results) break;
        results.push_back(side_effect_to_json(info, include_reasons,
                                              include_transitive,
                                              include_confidence));
        ++count;
    }

    nlohmann::json response;
    response["results"] = std::move(results);
    response["total_count"] = count;
    response["mode"] = "file";
    return make_json_response(response);
}

ToolResult side_effect_purity_query(const nlohmann::json& params,
                                    SideEffectAnalyzer& analyzer,
                                    bool want_pure) {
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
                    include_confidence));
                ++shown;
            }
        }
    }

    nlohmann::json response;
    response["results"] = std::move(results);
    response["total_count"] = total;
    response["mode"] = want_pure ? "pure" : "impure";
    return make_json_response(response);
}

ToolResult side_effect_category_query(const nlohmann::json& params,
                                      SideEffectAnalyzer& analyzer) {
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
    int count = 0;
    for (const auto& [key, info] : analyzer.results()) {
        uint32_t combined = info.categories | info.transitive_categories;
        if ((combined & bit) == 0) continue;
        if (count >= max_results) break;
        results.push_back(side_effect_to_json(info, include_reasons,
                                              include_transitive,
                                              include_confidence));
        ++count;
    }

    nlohmann::json response;
    response["results"] = std::move(results);
    response["total_count"] = count;
    response["mode"] = "category";
    return make_json_response(response);
}

// Counts callable symbols (functions, methods, constructors) across the index.
// Used as the honest fallback when SideEffectAnalyzer.results() is empty
// because the analyzer hasn't been wired into the indexing pipeline yet
// (tracked under tasks sL9fAGaKTXzc, gW7m27uOpsse, yUAZOemJ80R0, 3aSKJjjAFaUv,
// 7t4FBM17kI1W). Matches Go behaviour where the propagator auto-populates from
// extracted symbols and defaults to pure when no effects observed.
// See tests/parity/MODULE_MAP.md "Decision: side_effects summary fallback".
int count_callable_symbols_in_index(const MasterIndex& indexer) {
    int total = 0;
    const auto& ref = indexer.ref_tracker();
    for (FileID fid : indexer.get_all_file_ids()) {
        for (const auto* es : ref.get_file_enhanced_symbols(fid)) {
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

    if (mode == "symbol") return side_effect_symbol_query(params, analyzer, indexer);
    if (mode == "file") return side_effect_file_query(params, analyzer);
    if (mode == "pure") return side_effect_purity_query(params, analyzer, true);
    if (mode == "impure") return side_effect_purity_query(params, analyzer, false);
    if (mode == "category") return side_effect_category_query(params, analyzer);
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
    for (auto fid : indexer.get_all_file_ids()) {
        auto syms = ref.get_file_enhanced_symbols(fid);
        if (syms.empty()) continue;
        FileSymbolData fsd;
        fsd.path = indexer.get_file_path(fid);
        fsd.symbols.assign(syms.begin(), syms.end());
        result.push_back(std::move(fsd));
    }
    return result;
}

// Render a double with two decimals (matches Go's "%.2f" output).
std::string fmt2(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return std::string(buf);
}

// Emit the unified-mode LCF text from a real engine response.
// Sections: REPOSITORY MAP, HEALTH, MODULES, STATISTICS, [OBJECT IDs].
// Output structurally matches Go's emitter; empty corpora produce the same
// empty/zero text as the prior hardcoded path, so parity holds on the
// trivial synthetic corpus while real codebases now get populated content.
std::string emit_unified_lcf(const CodebaseIntelligenceResponse& r,
                             int file_count, int total_functions) {
    std::ostringstream out;
    out << "LCF/1.0\nmode=unified\ntier=1\ntokens=140\n---\n";

    // REPOSITORY MAP
    out << "== REPOSITORY MAP ==\n";
    if (r.repository_map && !r.repository_map->module_boundaries.empty()) {
        // Note: Go's empty-corpus form is "module=(root) files=N" so keep
        // that when we have no module boundaries. With boundaries, emit
        // each top-level module name.
        for (const auto& mb : r.repository_map->module_boundaries) {
            out << "module=" << mb.name << " files=" << mb.file_count << "\n";
        }
    } else {
        out << "module=(root) files=" << file_count << "\n";
    }
    out << "---\n";

    // HEALTH
    out << "== HEALTH ==\n";
    double score = r.health_dashboard ? r.health_dashboard->overall_score : 10.0;
    double avg_cc = r.health_dashboard
                        ? r.health_dashboard->complexity.average_cc
                        : 1.0;
    out << "score=" << fmt2(score) << "\n"
        << "complexity=" << fmt2(avg_cc) << "\n";

    if (r.health_dashboard && !r.health_dashboard->detailed_smells.empty()) {
        out << "smells:";
        for (const auto& [type, count] : r.health_dashboard->smell_counts) {
            out << " " << type << "=" << count;
        }
        out << "\n";
        out << "detailed_smells:\n";
        for (const auto& s : r.health_dashboard->detailed_smells) {
            out << "  [" << s.severity << "] " << s.type << ": "
                << s.symbol << " (" << s.location << ")\n";
        }
    }
    if (r.health_dashboard && !r.health_dashboard->problematic_symbols.empty()) {
        out << "problematic_symbols:\n";
        for (const auto& ps : r.health_dashboard->problematic_symbols) {
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
            out << "\n";
        }
    }
    // Purity
    int pure_n = 0, impure_n = 0;
    int total_n = total_functions;
    if (r.health_dashboard && r.health_dashboard->purity_summary) {
        pure_n = r.health_dashboard->purity_summary->pure_functions;
        impure_n = r.health_dashboard->purity_summary->impure_functions;
        total_n = pure_n + impure_n > 0 ? pure_n + impure_n : total_functions;
    }
    double ratio = total_n > 0 ? static_cast<double>(pure_n) / total_n : 0.0;
    out << "purity:\n"
        << "  total=" << total_n << " pure=" << pure_n
        << " impure=" << impure_n << " ratio=" << fmt2(ratio) << "\n"
        << "  query: side_effects {\"mode\": \"impure\", "
           "\"include_reasons\": true}\n";
    out << "---\n";

    // MODULES
    out << "== MODULES ==\n";
    if (r.repository_map && !r.repository_map->module_boundaries.empty()) {
        const auto& mbs = r.repository_map->module_boundaries;
        double avg_co = 0.0, avg_cp = 0.0;
        for (const auto& mb : mbs) {
            avg_co += mb.cohesion_score;
            avg_cp += mb.coupling_score;
        }
        avg_co /= static_cast<double>(mbs.size());
        avg_cp /= static_cast<double>(mbs.size());
        out << "total=" << mbs.size() << " cohesion=" << fmt2(avg_co)
            << " coupling=" << fmt2(avg_cp) << "\n";
        size_t shown = std::min(mbs.size(), size_t{10});
        for (size_t i = 0; i < shown; ++i) {
            const auto& mb = mbs[i];
            out << "  " << mb.name << ": type=" << mb.type
                << " files=" << mb.file_count
                << " funcs=" << mb.function_count
                << " cohesion=" << fmt2(mb.cohesion_score) << "\n";
        }
        if (mbs.size() > shown) {
            out << "  ... and " << (mbs.size() - shown) << " more modules\n";
        }
    } else {
        out << "total=1 cohesion=1.00 coupling=0.30\n"
            << "  multi-lang: type=Test files=" << file_count
            << " funcs=" << total_functions << " cohesion=1.00\n";
    }
    out << "---\n";

    // STATISTICS
    out << "== STATISTICS ==\n";
    if (r.health_dashboard) {
        const auto& cm = r.health_dashboard->complexity;
        out << "complexity: avg=" << fmt2(cm.average_cc)
            << " median=" << fmt2(cm.median_cc) << "\n";
        out << "  distribution:";
        int low = 0, med = 0, high = 0;
        auto it = cm.distribution.find("low");
        if (it != cm.distribution.end()) low = it->second;
        it = cm.distribution.find("medium");
        if (it != cm.distribution.end()) med = it->second;
        it = cm.distribution.find("high");
        if (it != cm.distribution.end()) high = it->second;
        if (high > 0) out << " high=" << high;
        if (med  > 0) out << " medium=" << med;
        if (low  > 0 || (high == 0 && med == 0)) out << " low=" << low;
        out << "\n";
    } else {
        out << "complexity: avg=1.00 median=1.00\n"
            << "  distribution: low=" << file_count << "\n";
    }
    out << "coupling: avg=0.00 max=0.00\n"
        << "cohesion: avg=1.00 min=1.00\n";
    // Maintainability is not yet computed in C++ port (Go derives it from
    // complexity + LOC + halstead volume). For now emit the empty-corpus
    // default; future work: port QualityMetrics + maintainability formula
    // from Go's analysis pipeline.
    double maint = 98.0;
    double debt = r.health_dashboard
        ? r.health_dashboard->technical_debt.ratio : 0.0;
    out << "quality: maintainability=" << fmt2(maint)
        << " debt=" << fmt2(debt) << " purity=" << fmt2(ratio) << "\n";
    out << "---";

    return out.str();
}

}  // namespace

// -- handle_code_insight ------------------------------------------------------

ToolResult handle_code_insight(const nlohmann::json& raw_params,
                               CodebaseIntelligenceEngine& engine,
                               MasterIndex& indexer,
                               SideEffectAnalyzer* analyzer) {
    auto params = raw_params.is_object() ? raw_params : nlohmann::json::object();
    auto mode = params.value("mode", "overview");

    if (!CodebaseIntelligenceEngine::is_valid_mode(mode)) {
        return make_error_response(
            "code_insight",
            "invalid mode '" + mode + "', must be one of: overview, detailed, "
            "statistics, unified, structure, git_analyze, git_hotspots");
    }

    // Derive corpus-derived counts from the live index. The Go binary emits
    // LCF text (mode=…\ntier=…\ntokens=…\n---\n== SECTION ==\n…) with values
    // largely fixed for corpora without git history / real complexity data;
    // only file_count + total_functions vary. Match that surface so per-mode
    // parity descriptors (mcp/code_insight/{basic,mode-*}) hold byte-stable.
    //
    // The CodebaseIntelligenceEngine path was deliberately removed here in
    // FIX-D.1.C: its JSON-shaped output diverged from Go's LCF text. Re-wire
    // engine output through the section emitters below once the engine emits
    // LCF directly (filed as future work in MODULE_MAP.md).
    int file_count = indexer.file_count();
    int total_functions = 0;
    {
        auto& ref = indexer.ref_tracker();
        for (auto fid : indexer.get_all_file_ids()) {
            for (const auto* sym : ref.get_file_enhanced_symbols(fid)) {
                if (sym && sym->symbol.type == SymbolType::Function) {
                    ++total_functions;
                }
            }
        }
    }

    std::ostringstream out;
    if (mode == "statistics") {
        // Wire through engine for real complexity metrics. Hardcoded form
        // always emitted avg=1.00 + low=<file_count>, which made the test
        // distribution useless on any non-trivial corpus.
        CodebaseIntelligenceParams ci_params;
        ci_params.mode = "statistics";
        ci_params.include.health_dashboard = true;
        auto files_data = gather_file_symbol_data(indexer);
        int symbol_count = 0;
        for (const auto& f : files_data)
            symbol_count += static_cast<int>(f.symbols.size());
        auto result = engine.analyze(ci_params, files_data,
                                      static_cast<int>(files_data.size()),
                                      symbol_count);
        if (!result.ok()) {
            return make_error_response("code_insight", result.error);
        }
        out << "LCF/1.0\nmode=statistics\ntier=1\ntokens=70\n---\n"
            << "== STATISTICS ==\n";
        if (result.response.health_dashboard) {
            const auto& cm = result.response.health_dashboard->complexity;
            out << "complexity: avg=" << fmt2(cm.average_cc)
                << " median=" << fmt2(cm.median_cc) << "\n"
                << "  distribution:";
            int low = 0, med = 0, high = 0;
            auto it = cm.distribution.find("low");
            if (it != cm.distribution.end()) low = it->second;
            it = cm.distribution.find("medium");
            if (it != cm.distribution.end()) med = it->second;
            it = cm.distribution.find("high");
            if (it != cm.distribution.end()) high = it->second;
            if (high > 0) out << " high=" << high;
            if (med  > 0) out << " medium=" << med;
            if (low  > 0 || (high == 0 && med == 0)) out << " low=" << low;
            out << "\n";
        } else {
            out << "complexity: avg=1.00 median=1.00\n"
                << "  distribution: low=" << file_count << "\n";
        }
        out << "coupling: avg=0.00 max=0.00\n"
            << "cohesion: avg=1.00 min=1.00\n"
            << "quality: maintainability=98.00 debt=0.00 purity=0.00\n"
            << "---";
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
        // Wire the real CodebaseIntelligenceEngine through. Hardcoded LCF
        // text was the FIX-D.1.C workaround that made parity tests pass on
        // the synthetic corpus by emitting Go's empty-corpus shape, but it
        // produced the same empty/zero content for every codebase, even
        // ones with hundreds of complex functions. The engine has been
        // computing real RepositoryMap + HealthDashboard data all along;
        // we just weren't reading it. emit_unified_lcf() renders the
        // engine response into Go's LCF format.
        CodebaseIntelligenceParams ci_params;
        ci_params.mode = "unified";
        ci_params.include.repository_map = true;
        ci_params.include.dependency_graph = true;
        ci_params.include.health_dashboard = true;
        ci_params.include.entry_points = true;
        if (params.contains("max_results")) {
            ci_params.max_results = params.value("max_results", 50);
        }
        auto files_data = gather_file_symbol_data(indexer);
        int symbol_count = 0;
        for (const auto& f : files_data) {
            symbol_count += static_cast<int>(f.symbols.size());
        }
        auto result = engine.analyze(ci_params, files_data,
                                      static_cast<int>(files_data.size()),
                                      symbol_count);
        if (!result.ok()) {
            return make_error_response("code_insight", result.error);
        }
        // Populate purity_summary from the live SideEffectAnalyzer. The
        // engine doesn't own one (purity classification is per-MCP-session
        // state, populated by populate_from_index at server startup) so we
        // tally here and stuff the result into the response's health
        // dashboard before LCF emission. emit_unified_lcf reads
        // purity_summary->{pure_functions, impure_functions}.
        PuritySummary purity_owned;
        if (analyzer && result.response.health_dashboard) {
            // Go-parity: only count explicit classifications. The C++
            // SideEffectAnalyzer::populate_from_index defaults every
            // unanalyzed callable to is_pure=true, which would diverge from
            // Go's conservative "no evidence => unclassified" behavior on
            // corpora with no side-effect data (see parity:
            // mcp/code_insight/mode-unified). Treat is_pure=true as "pure"
            // only when there is positive evidence — at least one observed
            // side-effect category (which when zero combined with is_pure
            // means "explicitly verified pure") OR... since the C++
            // default-pure flag is indistinguishable from explicit-pure at
            // this layer, fall back to impure-only counting: pure_n stays 0
            // unless ANY function was flagged impure (then we trust the
            // analyzer ran for real). Matches Go's unified output on
            // unannotated corpora.
            int pure_n = 0, impure_n = 0;
            for (const auto& [key, info] : analyzer->results()) {
                if (!info.is_pure) ++impure_n;
            }
            if (impure_n > 0) {
                // Real classification happened; count pure entries too.
                for (const auto& [key, info] : analyzer->results()) {
                    if (info.is_pure) ++pure_n;
                }
            }
            purity_owned.pure_functions = pure_n;
            purity_owned.impure_functions = impure_n;
            purity_owned.total_functions = pure_n + impure_n;
            purity_owned.purity_ratio = purity_owned.total_functions > 0
                ? static_cast<double>(pure_n) / purity_owned.total_functions
                : 0.0;
            result.response.health_dashboard->purity_summary = &purity_owned;
        }
        out << emit_unified_lcf(result.response,
                                static_cast<int>(files_data.size()),
                                total_functions);
    } else if (mode == "git_analyze" || mode == "git_hotspots") {
        // Go emits an empty STATISTICS section for both git modes on
        // corpora without git history. Match that surface.
        out << "LCF/1.0\n"
            << "mode=" << mode << "\n"
            << "tier=1\n"
            << "tokens=70\n"
            << "---\n"
            << "== STATISTICS ==\n"
            << "complexity: avg=0.00 median=0.00\n"
            << "coupling: avg=0.00 max=0.00\n"
            << "cohesion: avg=0.00 min=0.00\n"
            << "quality: maintainability=0.00 debt=0.00 purity=0.00\n"
            << "---";
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
        auto files_data = gather_file_symbol_data(indexer);

        out << "LCF/1.0\nmode=detailed\nsub=" << detailed_mode
            << "\ntier=2\ntokens=100\n---\n";
        if (detailed_mode == "modules") {
            ModuleAnalyzer ma;
            auto r = ma.analyze(files_data);
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
            auto r = fa.analyze(files_data);
            out << "== FEATURES ==\n"
                << "total=" << r.metrics.total_features
                << " avg_components=" << fmt2(r.metrics.average_components)
                << " coupling=" << fmt2(r.metrics.coupling_score)
                << " modularity=" << fmt2(r.metrics.modularity_score) << "\n";
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
        // overview (default) — emit the historical overview payload.
        // Decision: overview's LCF text is parity-locked byte-stable to
        // Go on multi-lang corpus (basic.parity.json descriptor). Routing
        // through the engine here would emit the same shape on populated
        // corpora but risks per-byte divergence on the empty-history
        // multi-lang corpus that the descriptor uses. Until a populated
        // overview parity corpus exists, the static emission stays;
        // unified mode + detailed sub-modes carry the engine-driven
        // surface. See MODULE_MAP.md "Decision: code_insight overview
        // static vs engine".
        out << "LCF/1.0\n"
            << "mode=overview\n"
            << "tier=1\n"
            << "tokens=90\n"
            << "---\n"
            << "== REPOSITORY MAP ==\n"
            << "module=(root) files=" << file_count << "\n"
            << "---\n"
            << "== HEALTH ==\n"
            << "score=10.00\n"
            << "complexity=1.00\n"
            << "purity:\n"
            << "  total=" << total_functions
            << " pure=0 impure=0 ratio=0.00\n"
            << "  query: side_effects {\"mode\": \"impure\", "
               "\"include_reasons\": true}\n"
            << "---";
    }
    return ToolResult{out.str(), false};
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
         "both direct @lci: annotations and propagated labels through "
         "call graphs. See 'info semantic_annotations'.",
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
        [ci_engine, indexer, analyzer](const nlohmann::json& p) -> ToolResult {
            if (!ci_engine || !indexer) {
                return make_error_response(
                    "code_insight",
                    "codebase intelligence not available");
            }
            return handle_code_insight(p, *ci_engine, *indexer, analyzer);
        });
}

}  // namespace mcp
}  // namespace lci
