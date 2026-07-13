// C++ port of the semantic section of internal/core/context_lookup.go
// (context_lookup_semantic.go: fillSemanticContext + its live getters).
// GraphPropagator/SemanticAnnotator are OPTIONAL collaborators (see
// ContextLookupEngine::set_graph_propagator / set_semantic_annotator) —
// every getter here degrades to an empty/default result when either is
// nullptr, matching Go's `if cle.graphPropagator != nil` /
// `if cle.semanticAnnotator == nil { return empty }` gates.
//
// Traps preserved bug-for-bug (see internal/core/context_lookup_semantic.go
// and the CLX port map, comment 01KXCYNRCFJECKP5EVTZ9A75B7):
//   trap 6b — the service-dependency matcher always reports the FIRST
//             service-pattern's operation ("api_call") regardless of which
//             hardcoded substring check actually fired: Go's inner
//             per-pattern loop body never tests `servicePattern.pattern`,
//             so the match condition is identical on every iteration and
//             the loop always records servicePatterns[0].operation before
//             breaking. TODO(clx-port): this is Go's bug
//             (context_lookup_semantic.go:139-156), ported verbatim rather
//             than "fixed" to report the pattern that conceptually fired.
//   trap 8  — analyzeCriticality reuses the propagation labels this fill
//             already computed (see fill_semantic_context) instead of
//             recomputing them a second time. Go calls getPropagationLabels
//             again inside analyzeCriticality (context_lookup_semantic.go:
//             227) — a redundant double-compute this port does not
//             replicate; caching/reusing satisfies the same observable
//             result without the wasted work.
//
// Divergence NOT replicated (not a named trap): Go's getEntryPointDependencies
// gates its entire body behind `cle.graphPropagator != nil`
// (context_lookup_semantic.go:76) even though findAllEntryPoints /
// canEntryPointReachObject only ever read the ref tracker / symbol index —
// graphPropagator is never touched by that path. Replicating the gate would
// silently zero out entry_point_dependencies for any caller that wires only
// a SemanticAnnotator (a supported, independent optional collaborator per
// this task's spec), so this port does not condition entry-point resolution
// on graph_propagator presence.

#include <lci/core/context_lookup.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_set.h>

#include <lci/core/graph_propagator.h>
#include <lci/core/reference_tracker.h>
#include <lci/core/semantic_annotator.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/reference.h>
#include <lci/symbol.h>

namespace lci {

namespace {

using Snapshot = ReferenceTracker::Snapshot;
using SymbolHandle = Snapshot::SymbolHandle;

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

// Resolves the target symbol by (name, file, type) — same lookup shape as
// the sibling S4/S5 files' resolve_target/find_target.
SymbolHandle find_target(const Snapshot& snap, const CodeObjectID& oid) {
    for (const auto& sym : snap.find_symbols_by_name(oid.name)) {
        if (sym->symbol.file_id != oid.file_id) continue;
        if (sym->symbol.type != oid.type) continue;
        return sym;
    }
    return nullptr;
}

// -- findAllEntryPoints (context_lookup_semantic.go:325) ---------------------
// Pattern 1: every "main" function. Pattern 2: function/method names
// containing a handler/endpoint keyword (case-insensitive substring — Go's
// symbolIndex.FindDefinitions fuzzy search has no live-index equivalent here,
// so every indexed function/method name is scanned directly instead).
// Pattern 3: symbols annotated @lci:category[entry-point] via the (optional)
// SemanticAnnotator. Dedup'd by (file_id, line), mirroring Go's LocationKey.
std::vector<CodeObjectID> find_all_entry_points(const Snapshot& snap,
                                                MasterIndex& indexer,
                                                SemanticAnnotator* annotator) {
    std::vector<CodeObjectID> entry_points;
    absl::flat_hash_set<uint64_t> seen;
    auto mark = [&](FileID file_id, int line) {
        uint64_t key =
            (static_cast<uint64_t>(file_id) << 32) | static_cast<uint32_t>(line);
        return seen.insert(key).second;
    };

    auto files = indexer.read_snapshot();
    for (const auto& [path, file_id] : files->file_map) {
        for (const auto& sym : snap.get_file_enhanced_symbols(file_id)) {
            if (sym->symbol.type != SymbolType::Function &&
                sym->symbol.type != SymbolType::Method) {
                continue;
            }
            std::string name_lower = lower(sym->symbol.name);
            bool is_main = sym->symbol.name == "main";
            bool is_handler = name_lower.find("handler") != std::string::npos ||
                              name_lower.find("endpoint") != std::string::npos;
            if (!is_main && !is_handler) continue;
            if (!mark(file_id, sym->symbol.line)) continue;

            CodeObjectID oid;
            oid.file_id = file_id;
            oid.symbol_id = encode_symbol_id(sym->id);
            oid.name = std::string(sym->symbol.name);
            oid.type = sym->symbol.type;
            entry_points.push_back(std::move(oid));
        }
    }

    if (annotator != nullptr) {
        for (const auto* annotated :
            annotator->get_symbols_by_category("entry-point")) {
            if (!mark(annotated->file_id, annotated->line)) continue;

            CodeObjectID oid;
            oid.file_id = annotated->file_id;
            oid.symbol_id = encode_symbol_id(annotated->symbol_id);
            oid.name = annotated->name;
            // AnnotatedSymbol does not carry the underlying symbol's type;
            // resolve it from the index (Go copies annotated.Symbol.Type
            // verbatim from its already-typed struct).
            if (auto handle = snap.get_enhanced_symbol(annotated->symbol_id)) {
                oid.type = handle->symbol.type;
            } else {
                oid.type = SymbolType::Function;
            }
            entry_points.push_back(std::move(oid));
        }
    }

    return entry_points;
}

// -- canEntryPointReachObject (context_lookup_semantic.go:400) --------------
// BFS forward from entry via callee names (call-graph edges), checking
// equality against target_name at dequeue time.
bool can_entry_point_reach(const Snapshot& snap, ReferenceTracker& tracker,
                           const std::string& entry_name,
                           const std::string& target_name) {
    if (snap.find_symbols_by_name(entry_name).empty()) return false;

    absl::flat_hash_set<std::string> visited;
    std::vector<std::string> queue{entry_name};
    size_t head = 0;
    while (head < queue.size()) {
        std::string current = std::move(queue[head++]);
        if (current == target_name) return true;
        if (!visited.insert(current).second) continue;

        auto current_syms = snap.find_symbols_by_name(current);
        if (current_syms.empty()) continue;
        for (const auto& callee :
            tracker.get_callee_names(current_syms.front()->id)) {
            if (!visited.contains(callee)) queue.push_back(callee);
        }
    }
    return false;
}

std::string determine_entry_point_type(const CodeObjectID& entry) {
    std::string name = lower(entry.name);
    if (name == "main" || name == "_main") return "main function";
    if (name.find("handler") != std::string::npos ||
        name.find("serve") != std::string::npos) {
        return "HTTP endpoint";
    }
    if (name.find("cmd") != std::string::npos ||
        name.find("command") != std::string::npos) {
        return "CLI command";
    }
    if (name.find("test") != std::string::npos) return "test entry point";
    return "unknown entry point";
}

// extractPathFromDocComment (context_lookup_semantic.go:490) — pulls a
// route/command annotation from the first matching doc-comment line.
std::string extract_path_from_doc_comment(const std::string& doc_comment) {
    if (doc_comment.empty()) return "";

    static constexpr std::string_view kMethods[] = {"GET",  "POST", "PUT",
                                                     "DELETE", "PATCH", "WS"};

    std::istringstream stream(doc_comment);
    std::string line;
    while (std::getline(stream, line)) {
        size_t begin = line.find_first_not_of(" \t\r");
        if (begin == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r");
        std::string trimmed = line.substr(begin, end - begin + 1);
        if (trimmed.empty()) continue;

        for (auto method : kMethods) {
            std::string prefix = std::string(method) + " ";
            if (trimmed.rfind(prefix, 0) == 0) return trimmed;
        }
        if (trimmed.rfind("@route ", 0) == 0) return trimmed.substr(7);
        if (trimmed.rfind("Command:", 0) == 0) {
            std::string cmd = trimmed.substr(8);
            size_t cb = cmd.find_first_not_of(" \t");
            return cb == std::string::npos ? "" : cmd.substr(cb);
        }
    }
    return "";
}

std::string get_entry_point_path(const Snapshot& snap,
                                 const CodeObjectID& entry) {
    auto target = find_target(snap, entry);
    if (target == nullptr) return "";
    return extract_path_from_doc_comment(target->doc_comment);
}

// -- getEntryPointDependencies (context_lookup_semantic.go:72) --------------
std::vector<EntryPointRef> get_entry_point_dependencies(
    const Snapshot& snap, ReferenceTracker& tracker, MasterIndex& indexer,
    SemanticAnnotator* annotator, const CodeObjectID& oid) {
    std::vector<EntryPointRef> result;
    for (const auto& ep : find_all_entry_points(snap, indexer, annotator)) {
        if (!can_entry_point_reach(snap, tracker, ep.name, oid.name)) continue;

        EntryPointRef ref;
        ref.entry_point_id = ep;
        ref.type = determine_entry_point_type(ep);
        ref.path = get_entry_point_path(snap, ep);
        ref.confidence = 0.8;
        result.push_back(std::move(ref));
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const EntryPointRef& a, const EntryPointRef& b) {
                         return a.confidence > b.confidence;
                     });
    return result;
}

// -- getServiceDependencies (context_lookup_semantic.go:99) -----------------
std::vector<ServiceRef> get_service_dependencies(const Snapshot& snap,
                                                 const CodeObjectID& oid) {
    std::vector<ServiceRef> services;
    auto target = find_target(snap, oid);
    if (target == nullptr) return services;

    for (const auto& ref : target->outgoing_refs) {
        if (ref.type != ReferenceType::Call) continue;
        const std::string& call_name = ref.referenced_name;
        // trap 6b: hardcoded substring check, independent of any
        // servicePatterns entry — see file-level comment.
        bool matches = call_name.find("connect") != std::string::npos ||
                      call_name.find("dial") != std::string::npos ||
                      call_name.find("query") != std::string::npos ||
                      call_name.find("execute") != std::string::npos;
        if (!matches) continue;

        ServiceRef svc;
        svc.service_name = call_name;
        svc.operation_type = "api_call";  // trap 6b: always servicePatterns[0].
        svc.dependency_type = "direct";
        svc.confidence = 0.6;
        services.push_back(std::move(svc));
    }

    absl::flat_hash_set<std::string> seen;
    std::vector<ServiceRef> unique;
    unique.reserve(services.size());
    for (auto& svc : services) {
        std::string key = svc.service_name + '\0' + svc.operation_type;
        if (seen.insert(key).second) unique.push_back(std::move(svc));
    }
    return unique;
}

// -- getPropagationLabels (context_lookup_semantic.go:165) ------------------
std::vector<PropagationInfo> get_propagation_labels(
    const Snapshot& snap, const CodeObjectID& oid, GraphPropagator* propagator,
    SemanticAnnotator* annotator) {
    std::vector<PropagationInfo> labels;
    auto target = find_target(snap, oid);

    if (propagator != nullptr && target != nullptr) {
        for (const auto& propagated : propagator->get_labels(target->id)) {
            PropagationInfo info;
            info.label = propagated.label;
            info.strength = propagated.strength;
            info.direction = propagated.hops > 0 ? "upstream" : "bidirectional";
            info.source.symbol_id = encode_symbol_id(propagated.source);
            if (auto source_handle = snap.get_enhanced_symbol(propagated.source)) {
                info.source.file_id = source_handle->symbol.file_id;
                info.source.name = std::string(source_handle->symbol.name);
                info.source.type = source_handle->symbol.type;
            }
            labels.push_back(std::move(info));
        }
    }

    // Direct @lci: annotations on this object: full strength, bidirectional.
    // SemanticAnnotator keys its internal map by a SYNTHETIC id —
    // (file_id<<32 | line<<16 | column) — computed at extraction time
    // (semantic_annotator.cpp's extract_annotations), NOT the real indexed
    // EnhancedSymbol::id used everywhere else in this file. The lookup key
    // must be rebuilt the same way or get_annotation() silently misses.
    if (annotator != nullptr && target != nullptr) {
        SymbolID annotation_key =
            (static_cast<SymbolID>(oid.file_id) << 32) |
            (static_cast<SymbolID>(target->symbol.line) << 16) |
            static_cast<SymbolID>(target->symbol.column);
        if (const auto* annotation =
                annotator->get_annotation(oid.file_id, annotation_key)) {
            for (const auto& label : annotation->labels) {
                PropagationInfo info;
                info.label = label;
                info.source = oid;
                info.strength = 1.0;
                info.direction = "bidirectional";
                labels.push_back(std::move(info));
            }
        }
    }

    return labels;
}

// -- analyzeCriticality helpers (context_lookup_semantic.go:597) ------------

std::string determine_criticality_type(const std::string& label) {
    if (label.find("security") != std::string::npos) return "security";
    if (label.find("performance") != std::string::npos) return "performance";
    if (label.find("bug") != std::string::npos) return "correctness";
    return "general";
}

double calculate_impact_score(double strength, const std::string& direction) {
    double score = strength * 10.0;
    if (direction == "bidirectional") score *= 1.2;
    return std::min(score, 10.0);
}

bool is_security_critical(const CodeObjectID& oid) {
    std::string name = lower(oid.name);
    static constexpr std::string_view kKeywords[] = {
        "auth", "password", "token",   "encrypt",
        "decrypt", "hash",     "validate", "sanitize"};
    for (auto keyword : kKeywords) {
        if (name.find(keyword) != std::string::npos) return true;
    }
    return false;
}

bool is_performance_critical(const Snapshot& snap, ReferenceTracker& tracker,
                             const CodeObjectID& oid) {
    auto target = find_target(snap, oid);
    if (target == nullptr) return false;
    constexpr size_t kHotPathThreshold = 100;
    return tracker.get_caller_names(target->id).size() >= kHotPathThreshold;
}

bool processes_business_logic(const Snapshot& snap, const CodeObjectID& oid) {
    if (oid.type != SymbolType::Function && oid.type != SymbolType::Method) {
        return false;
    }
    auto target = find_target(snap, oid);
    if (target == nullptr) return false;
    if (target->complexity > 5) return true;

    int call_count = 0;
    for (const auto& ref : target->outgoing_refs) {
        if (ref.type == ReferenceType::Call) ++call_count;
    }
    return call_count > 3;
}

bool is_business_logic_critical(const Snapshot& snap, const CodeObjectID& oid) {
    if (oid.type != SymbolType::Function && oid.type != SymbolType::Method) {
        return false;
    }
    std::string name = lower(oid.name);
    static constexpr std::string_view kKeywords[] = {
        "payment", "price",  "discount",   "authorize",   "validate", "order",
        "calculate", "process", "approve",    "charge",     "refund",  "invoice",
        "bill",    "transaction", "apply",   "check",       "verify"};
    bool has_business_naming = false;
    for (auto keyword : kKeywords) {
        if (name.find(keyword) != std::string::npos) {
            has_business_naming = true;
            break;
        }
    }
    if (!has_business_naming) return false;
    return processes_business_logic(snap, oid);
}

std::string extract_component_name(const std::string& function_name) {
    std::string name = lower(function_name);
    static constexpr std::string_view kPatterns[] = {
        "handler", "handle",    "repository", "repo",  "service", "controller",
        "api",     "web",       "database",   "db",    "cache",   "util",
        "helper"};
    std::string best;
    long best_pos = -1;
    for (auto pattern : kPatterns) {
        auto pos = name.find(pattern);
        if (pos != std::string::npos && static_cast<long>(pos) > best_pos) {
            best_pos = static_cast<long>(pos);
            best = std::string(pattern);
        }
    }
    return best;
}

std::vector<std::string> get_affected_components(const Snapshot& snap,
                                                  ReferenceTracker& tracker,
                                                  const CodeObjectID& oid) {
    std::vector<std::string> components;
    auto target = find_target(snap, oid);
    if (target == nullptr) return components;

    absl::flat_hash_set<std::string> seen;
    for (const auto& caller_name : tracker.get_caller_names(target->id)) {
        std::string component = extract_component_name(caller_name);
        if (!component.empty() && seen.insert(component).second) {
            components.push_back(component);
        }
    }
    return components;
}

// trap 8: reuses `labels` (already computed by fill_semantic_context via
// get_propagation_labels) instead of recomputing — see file-level comment.
CriticalityInfo analyze_criticality(const Snapshot& snap,
                                    ReferenceTracker& tracker,
                                    const CodeObjectID& oid,
                                    const std::vector<PropagationInfo>& labels) {
    CriticalityInfo info;

    for (const auto& label : labels) {
        if (label.label.find("critical") != std::string::npos ||
            label.label.find("security") != std::string::npos) {
            info.is_critical = true;
            info.criticality_type = determine_criticality_type(label.label);
            info.impact_score =
                calculate_impact_score(label.strength, label.direction);
            break;
        }
    }

    if (!info.is_critical) {
        if (is_security_critical(oid)) {
            info.is_critical = true;
            info.criticality_type = "security";
            info.impact_score = 8.0;
        } else if (is_performance_critical(snap, tracker, oid)) {
            info.is_critical = true;
            info.criticality_type = "performance";
            info.impact_score = 6.0;
        } else if (is_business_logic_critical(snap, oid)) {
            info.is_critical = true;
            info.criticality_type = "business-logic";
            info.impact_score = 7.0;
        }
    }

    if (info.is_critical) {
        info.affected_components = get_affected_components(snap, tracker, oid);
    }
    return info;
}

// -- determinePurpose (context_lookup_semantic.go:267) -----------------------

bool makes_api_calls(const Snapshot& snap, const CodeObjectID& oid) {
    if (oid.type != SymbolType::Function && oid.type != SymbolType::Method) {
        return false;
    }
    auto target = find_target(snap, oid);
    if (target == nullptr) return false;
    for (const auto& ref : target->outgoing_refs) {
        if (ref.type != ReferenceType::Call) continue;
        std::string call_name = lower(ref.referenced_name);
        if (call_name.find("http") != std::string::npos ||
            call_name.find("get") != std::string::npos ||
            call_name.find("post") != std::string::npos ||
            call_name.find("request") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool accesses_database(const Snapshot& snap, const CodeObjectID& oid) {
    if (oid.type != SymbolType::Function && oid.type != SymbolType::Method) {
        return false;
    }
    auto target = find_target(snap, oid);
    if (target == nullptr) return false;
    for (const auto& ref : target->outgoing_refs) {
        if (ref.type != ReferenceType::Call) continue;
        std::string call_name = lower(ref.referenced_name);
        if (call_name.find("sql") != std::string::npos ||
            call_name.find("query") != std::string::npos ||
            call_name.find("exec") != std::string::npos ||
            call_name.find("db") != std::string::npos ||
            call_name.find("database") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::pair<std::string, double> determine_purpose(const Snapshot& snap,
                                                  const CodeObjectID& oid) {
    std::string name = lower(oid.name);

    if (name.find("handler") != std::string::npos ||
        name.find("controller") != std::string::npos) {
        return {"API handler", 0.9};
    }
    if (name.find("middleware") != std::string::npos) {
        return {"middleware", 0.9};
    }
    if (name.find("util") != std::string::npos ||
        name.find("helper") != std::string::npos) {
        return {"utility function", 0.8};
    }
    if (name.find("config") != std::string::npos ||
        name.find("settings") != std::string::npos) {
        return {"configuration", 0.9};
    }
    if (name.find("test") != std::string::npos ||
        name.find("spec") != std::string::npos) {
        return {"test", 0.9};
    }
    if (name.find("model") != std::string::npos ||
        name.find("entity") != std::string::npos) {
        return {"data model", 0.8};
    }
    if (name.find("service") != std::string::npos) {
        return {"service layer", 0.8};
    }
    if (name.find("repo") != std::string::npos ||
        name.find("repository") != std::string::npos) {
        return {"data access", 0.8};
    }

    if (makes_api_calls(snap, oid)) return {"API client", 0.7};
    if (accesses_database(snap, oid)) return {"data access", 0.7};
    if (processes_business_logic(snap, oid)) return {"business logic", 0.6};

    switch (oid.type) {
        case SymbolType::Function:
            return {"function", 0.5};
        case SymbolType::Class:
            return {"class", 0.5};
        case SymbolType::Method:
            return {"method", 0.5};
        case SymbolType::Interface:
            return {"interface", 0.5};
        default:
            return {"unknown", 0.3};
    }
}

}  // namespace

// Populates ctx.semantic_context. Mirrors Go's fillSemanticContext dispatch,
// reading ctx.object_id (refreshed by fill_basic_info) as the objectID.
void fill_semantic_context(CodeObjectContext& ctx, const Snapshot& snap,
                           MasterIndex& indexer, GraphPropagator* propagator,
                           SemanticAnnotator* annotator) {
    const CodeObjectID& oid = ctx.object_id;
    SemanticContext& sc = ctx.semantic_context;
    ReferenceTracker& tracker = indexer.ref_tracker();

    sc.entry_point_dependencies =
        get_entry_point_dependencies(snap, tracker, indexer, annotator, oid);
    sc.service_dependencies = get_service_dependencies(snap, oid);
    sc.propagation_labels =
        get_propagation_labels(snap, oid, propagator, annotator);
    // trap 8: pass the labels just computed in rather than recomputing them.
    sc.criticality_analysis =
        analyze_criticality(snap, tracker, oid, sc.propagation_labels);

    auto [purpose, confidence] = determine_purpose(snap, oid);
    sc.purpose = purpose;
    sc.confidence = confidence;
}

}  // namespace lci
