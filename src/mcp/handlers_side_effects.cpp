#include <lci/mcp/handlers_side_effects.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

#include <lci/analysis/codebase_intelligence_types.h>
#include <lci/analysis/error_handling_analyzer.h>
#include <lci/analysis/side_effect_analyzer.h>
#include <lci/search/search_engine.h>
#include <lci/core/graph_propagator.h>
#include <lci/core/semantic_annotator.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/validation.h>
#include <lci/symbol.h>

namespace lci {
namespace mcp {

// -- Helpers ------------------------------------------------------------------

namespace {

// categories_to_strings is shared via side_effects.h (also used by
// get_context purity emission) — no local duplicate.

/// Canonical (name, bit) table for `side_effects {"mode":"category"}`. The
/// error message below enumerates this same table so it cannot drift from
/// what the lookup actually accepts (finding 9).
struct CategoryEntry {
    std::string_view canonical_name;
    uint32_t bit;
};
constexpr CategoryEntry kCategoryTable[] = {
    {"param_write", side_effect::kParamWrite},
    {"receiver_write", side_effect::kReceiverWrite},
    {"global_write", side_effect::kGlobalWrite},
    {"closure_write", side_effect::kClosureWrite},
    {"io", side_effect::kIO},
    {"database", side_effect::kDatabase},
    {"network", side_effect::kNetwork},
    {"throw", side_effect::kThrow},
    {"channel", side_effect::kChannel},
    {"external_call", side_effect::kExternalCall},
    {"dynamic_call", side_effect::kDynamicCall},
    {"uncertain", side_effect::kUncertain},
};

/// Maps a category name string to a side_effect bitfield constant.
uint32_t category_name_to_bit(std::string_view name) {
    // Normalise to lowercase
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    // Accept a few common spelling variants of the canonical name.
    std::string collapsed;
    collapsed.reserve(lower.size());
    for (char c : lower) {
        if (c == '-') continue;
        collapsed += (c == '_') ? '_' : c;
    }

    if (lower == "global") return side_effect::kGlobalWrite;
    if (lower == "closure") return side_effect::kClosureWrite;
    if (lower == "db") return side_effect::kDatabase;
    if (lower == "net") return side_effect::kNetwork;
    if (lower == "throws" || lower == "panic") return side_effect::kThrow;
    if (lower == "chan") return side_effect::kChannel;
    if (lower == "external") return side_effect::kExternalCall;
    if (lower == "dynamic") return side_effect::kDynamicCall;
    if (lower == "unknown") return side_effect::kUncertain;

    for (const auto& entry : kCategoryTable) {
        std::string no_sep(entry.canonical_name);
        no_sep.erase(std::remove(no_sep.begin(), no_sep.end(), '_'), no_sep.end());
        if (lower == entry.canonical_name || collapsed == no_sep) return entry.bit;
    }
    return side_effect::kNone;
}

/// Builds "the valid category list" clause for the unknown-category error,
/// enumerated straight from kCategoryTable so it cannot drift from what
/// category_name_to_bit actually accepts.
std::string valid_category_names() {
    std::string out;
    for (size_t i = 0; i < std::size(kCategoryTable); ++i) {
        if (i != 0) out += ", ";
        out += kCategoryTable[i].canonical_name;
    }
    return out;
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
        if (info.error_handling.catch_count > 0)
            item["catch_count"] = info.error_handling.catch_count;
        if (info.error_handling.returns_error)
            item["returns_error"] = true;
    }

    auto findings_json = [](const std::vector<EhFinding>& v) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& f : v) {
            nlohmann::json j;
            j["signal"] = std::string(to_string(f.signal));
            j["severity"] = std::string(to_string(f.severity));
            j["confidence"] = f.confidence;
            j["line"] = f.line;
            if (!f.detail.empty()) j["detail"] = f.detail;
            arr.push_back(std::move(j));
        }
        return arr;
    };
    if (!info.error_findings.empty())
        item["error_findings"] = findings_json(info.error_findings);
    if (!info.resource_findings.empty())
        item["resource_findings"] = findings_json(info.resource_findings);
    if (!info.resource_acquires.empty())
        item["resource_acquires"] =
            static_cast<int>(info.resource_acquires.size());
    if (!info.resource_releases.empty())
        item["resource_releases"] =
            static_cast<int>(info.resource_releases.size());

    return item;
}

/// JSON twin of the == ERROR HANDLING == / == RESOURCE MANAGEMENT ==
/// sections — err-lookup ingests JSON, never parses LCF.
nlohmann::json eh_findings_json(const std::vector<EhFindingEntry>& v) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& f : v) {
        nlohmann::json j;
        j["severity"] = f.severity;
        j["signal"] = f.signal;
        j["symbol"] = f.symbol;
        j["location"] = f.location;
        if (!f.object_id.empty()) j["object_id"] = f.object_id;
        if (!f.detail.empty()) j["detail"] = f.detail;
        j["confidence"] = f.confidence;
        arr.push_back(std::move(j));
    }
    return arr;
}

nlohmann::json error_handling_to_json(const ErrorHandlingSummary& s) {
    nlohmann::json j;
    j["score"] = s.score;
    j["functions_scored"] = s.functions_scored;
    j["throwers"] = s.throwers;
    j["handled_ratio"] = s.handled_ratio;
    j["swallow_sites"] = s.swallow_sites;
    if (s.suppressed > 0) j["suppressed"] = s.suppressed;
    j["unchecked_errors"] = s.unchecked_errors;
    j["uncompensated"] = s.uncompensated;
    j["irreversible_first"] = s.irreversible_first;
    j["findings"] = eh_findings_json(s.findings);
    return j;
}

nlohmann::json resources_to_json(const ResourceSummary& s) {
    nlohmann::json j;
    j["score"] = s.score;
    j["functions_scored"] = s.functions_scored;
    j["acquisitions"] = s.acquisitions;
    j["released_ratio"] = s.released_ratio;
    j["guarded_ratio"] = s.guarded_ratio;
    j["findings"] = eh_findings_json(s.findings);
    return j;
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

/// Collects analyzer records matching `keep`, sorted by (file_path,
/// start_line). analyzer.results() is a flat_hash_map whose iteration order
/// varies per process; every capped emission must sort before it truncates
/// (karpathy #4: never hash-iteration order in user-visible output).
template <typename Keep>
std::vector<const SideEffectInfo*> sorted_results(
    const SideEffectAnalyzer& analyzer, Keep keep) {
    std::vector<const SideEffectInfo*> out;
    out.reserve(analyzer.results().size());
    for (const auto& [key, info] : analyzer.results()) {
        if (keep(info)) out.push_back(&info);
    }
    std::sort(out.begin(), out.end(),
              [](const SideEffectInfo* a, const SideEffectInfo* b) {
                  if (a->file_path != b->file_path)
                      return a->file_path < b->file_path;
                  return a->start_line < b->start_line;
              });
    return out;
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
                "`@lci:label[...]` comments above symbols to populate them";
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
    auto symbol_id = params.value("symbol_id", "");
    auto file_path = params.value("file_path", "");

    if (symbol_name.empty() && symbol_id.empty() && file_path.empty()) {
        return make_error_response(
            "side_effects",
            "symbol mode requires 'symbol_name', 'symbol_id', or "
            "'file_path' with symbol lookup");
    }

    bool include_reasons = params.value("include_reasons", false);
    bool include_transitive = params.value("include_transitive", false);
    bool include_confidence = params.value("include_confidence", false);

    if (!indexer) {
        return make_error_response(
            "side_effects", "symbol lookup requires an index");
    }

    auto& ref = indexer->ref_tracker();
    auto rt_snap = ref.pin();

    const EnhancedSymbol* resolved = nullptr;
    ReferenceTracker::Snapshot::SymbolHandle handle;

    // symbol_id is the exact address — honor it first (the schema advertises
    // it; object ids from list_symbols/inspect_symbol are meant to be used).
    if (!symbol_id.empty()) {
        auto decoded = decode_symbol_id(symbol_id);
        if (!decoded) {
            return make_error_response(
                "side_effects", "invalid symbol_id: " + symbol_id);
        }
        handle = rt_snap->get_enhanced_symbol(*decoded);
        if (!handle) {
            return make_unavailable_response(
                "side_effects", "symbol_id not found: " + symbol_id,
                "the id may be stale; re-run list_symbols or inspect_symbol "
                "to get a current object_id");
        }
        resolved = handle.get();
    } else if (!symbol_name.empty()) {
        auto symbols = rt_snap->find_symbols_by_name(symbol_name);
        // file_path disambiguates same-name definitions: keep only the
        // symbols whose file matches (compared root-relative, like file
        // mode, so both spellings work).
        if (!file_path.empty()) {
            std::string want_rel(relative_to_root(file_path, root));
            std::vector<ReferenceTracker::Snapshot::SymbolHandle> in_file;
            for (const auto& s : symbols) {
                if (relative_to_root(
                        indexer->get_file_path(s->symbol.file_id), root) ==
                    want_rel) {
                    in_file.push_back(s);
                }
            }
            symbols = std::move(in_file);
        }
        if (symbols.empty()) {
            // Lookup miss: a definitive negative answer, not a tool error
            // (matches inspect_symbol's found=false shape).
            return make_unavailable_response(
                "side_effects", "symbol not found: " + symbol_name,
                "check the spelling, or use search/list_symbols to locate "
                "the symbol");
        }
        handle = symbols[0];
        resolved = handle.get();
    }

    if (resolved) {
        auto path = indexer->get_file_path(resolved->symbol.file_id);
        auto* info = analyzer.get_result(path, resolved->symbol.line,
                                          resolved->symbol.column);
        if (!info) {
            // Stay loud without the error flag: we resolved the symbol but
            // the analyzer holds no side-effect record — a bare empty results
            // array reads as "pure/no effects" and misleads. available=false
            // + reason says exactly why, without signaling a code failure.
            return make_unavailable_response(
                "side_effects",
                "symbol '" + resolved->symbol.name + "' resolved at " +
                    std::string(relative_to_root(path, root)) + ":" +
                    std::to_string(resolved->symbol.line) +
                    " but has no side-effect record (not a function/method, "
                    "or the analyzer is unpopulated for this corpus)",
                "try side_effects {\"mode\":\"summary\"}");
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
        "side_effects",
        "symbol lookup requires 'symbol_name' or 'symbol_id'");
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
    auto matched = sorted_results(analyzer, [&](const SideEffectInfo& info) {
        return relative_to_root(info.file_path, root) == want_rel;
    });
    const int total = static_cast<int>(matched.size());
    const int shown = std::min(total, max_results);
    results.get_ref<nlohmann::json::array_t&>().reserve(
        static_cast<size_t>(shown));
    for (int i = 0; i < shown; ++i) {
        results.push_back(side_effect_to_json(*matched[static_cast<size_t>(i)],
                                              include_reasons,
                                              include_transitive,
                                              include_confidence, root));
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
    auto matched = sorted_results(analyzer, [&](const SideEffectInfo& info) {
        return info.is_pure == want_pure;
    });
    const int total = static_cast<int>(matched.size());
    const int shown = std::min(total, max_results);
    results.get_ref<nlohmann::json::array_t&>().reserve(
        static_cast<size_t>(shown));
    for (int i = 0; i < shown; ++i) {
        results.push_back(side_effect_to_json(*matched[static_cast<size_t>(i)],
                                              include_reasons,
                                              include_transitive,
                                              include_confidence, root));
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
            "unknown category: " + category + " (valid: " +
            valid_category_names() + ")");
    }

    bool include_reasons = params.value("include_reasons", false);
    bool include_transitive = params.value("include_transitive", false);
    bool include_confidence = params.value("include_confidence", false);
    int max_results = clamp(params.value("max_results", 100), 1, 10000);

    nlohmann::json results = nlohmann::json::array();
    auto matched = sorted_results(analyzer, [&](const SideEffectInfo& info) {
        uint32_t combined = info.categories | info.transitive_categories;
        return (combined & bit) != 0;
    });
    const int total = static_cast<int>(matched.size());
    const int shown = std::min(total, max_results);
    results.get_ref<nlohmann::json::array_t&>().reserve(
        static_cast<size_t>(shown));
    for (int i = 0; i < shown; ++i) {
        results.push_back(side_effect_to_json(*matched[static_cast<size_t>(i)],
                                              include_reasons,
                                              include_transitive,
                                              include_confidence, root));
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

    // Fail loud on empty analysis (Karpathy #6, no fabricated data): an
    // analyzer with zero records means the side-effect pass produced nothing
    // for this corpus. Report the unavailability explicitly — never invent a
    // purity ratio from the index's callable count (that would claim 100%
    // pure for functions nobody analyzed).
    if (total == 0) {
        nlohmann::json response;
        response["results"] = nullptr;
        response["total_count"] = 0;
        response["mode"] = "summary";
        response["error"] = "analysis_unavailable";
        response["hint"] =
            "no per-function side-effect data for this corpus; the "
            "side-effect analyzer produced no records (unsupported languages "
            "or the analysis pass has not run)";
        return make_json_response(response);
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

    // Finding 7: fixpoint_truncated() previously had no reader anywhere —
    // surface it here so a caller can tell a partial transitive assessment
    // from a converged one instead of silently trusting an incomplete run.
    if (analyzer.fixpoint_truncated()) {
        response["fixpoint_truncated"] = true;
        response["fixpoint_max_iterations"] =
            SideEffectAnalyzer::kMaxPropagationIterations;
    }

    // Error-handling + resource rollups (JSON twin of the code_insight
    // sections; the natural err-lookup ingestion path).
    if (indexer != nullptr && !analyzer.results().empty() &&
        indexer->config().insight.error_report == "on") {
        auto eh = ErrorHandlingAnalyzer::analyze(
            analyzer, *indexer, indexer->config().project.root);
        response["error_handling"] = error_handling_to_json(eh.errors);
        response["resources"] = resources_to_json(eh.resources);
    }
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

}  // namespace mcp
}  // namespace lci
