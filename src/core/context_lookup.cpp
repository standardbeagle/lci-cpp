#include <lci/core/context_lookup.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

#include <absl/container/flat_hash_set.h>

#include <lci/core/portable.h>
#include <lci/core/reference_tracker.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/symbol.h>

namespace lci {

// Defined in context_lookup_relationships.cpp (CLX S3). Fills
// ctx.direct_relationships from the pinned snapshot plus the engine's live
// ReferenceTracker (needed for get_caller_symbols/get_callee_symbols) and the
// engine's current confidence_threshold().
void fill_direct_relationships(CodeObjectContext& ctx,
                               const ReferenceTracker::Snapshot& snap,
                               ReferenceTracker& tracker, double threshold);

// Defined in context_lookup_variables.cpp (CLX S4). Fills ctx.variable_context
// from the pinned snapshot. Declared here (not in the header) to keep
// context_lookup.h free of the heavy reference_tracker.h include; both TUs see
// ReferenceTracker::Snapshot, so the linker enforces the signature.
void fill_variable_context(CodeObjectContext& ctx,
                           const ReferenceTracker::Snapshot& snap);

// Defined in context_lookup_structure.cpp (CLX S5). Fills
// ctx.structure_context from the pinned snapshot plus the indexer (needed for
// file-path resolution).
void fill_structure_context(CodeObjectContext& ctx,
                            const ReferenceTracker::Snapshot& snap,
                            MasterIndex& indexer);

// Defined in context_lookup_semantic.cpp (CLX S6). Fills ctx.semantic_context
// from the pinned snapshot plus the indexer; propagator/annotator are
// OPTIONAL (nil-tolerant) collaborators the engine may not have wired.
void fill_semantic_context(CodeObjectContext& ctx,
                           const ReferenceTracker::Snapshot& snap,
                           MasterIndex& indexer, GraphPropagator* propagator,
                           SemanticAnnotator* annotator);

// Defined in context_lookup_usage.cpp (CLX S6). Fills ctx.usage_analysis from
// the pinned snapshot plus the indexer (needed for dependent-component module
// resolution); reads ctx.direct_relationships (already filled by S3) rather
// than re-deriving the caller list.
void fill_usage_analysis(CodeObjectContext& ctx,
                         const ReferenceTracker::Snapshot& snap,
                         MasterIndex& indexer);

// Defined in context_lookup_ai.cpp (CLX S8). Fills ctx.ai_context by reading
// sections already populated above (relationships/semantic/usage/structure)
// rather than recomputing anything. No snapshot/indexer needed. Gated
// wholesale on `include_ai_text` — false leaves the whole section empty.
void fill_ai_context(CodeObjectContext& ctx, bool include_ai_text);

namespace {

// Formats now() as an RFC3339 UTC timestamp. generated_at is time.Now() in Go
// and never appears in a deterministic golden, so a UTC "Z" form is sufficient
// (the local-offset form Go emits is not load-bearing for parity).
std::string now_rfc3339_utc() {
    auto tp = std::chrono::system_clock::now();
    auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp);
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp - secs)
                  .count();
    std::time_t t = std::chrono::system_clock::to_time_t(secs);
    std::tm tm{};
    if (!portable::gmtime_utc(t, tm)) return std::string{};
    char buf[48];
    int n = std::snprintf(buf, sizeof(buf),
                          "%04d-%02d-%02dT%02d:%02d:%02d.%09ldZ",
                          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                          tm.tm_hour, tm.tm_min, tm.tm_sec,
                          static_cast<long>(ns));
    if (n <= 0) return std::string{};
    return std::string(buf, static_cast<size_t>(n));
}

}  // namespace

bool ContextLookupEngine::fill_basic_info(CodeObjectContext& ctx,
                                          const EnhancedSymbol& sym) const {
    // Refresh identity from the authoritative symbol (the caller-supplied
    // CodeObjectID may carry a stale name/type).
    ctx.object_id.file_id = sym.symbol.file_id;
    ctx.object_id.name = std::string(sym.symbol.name);
    ctx.object_id.type = sym.symbol.type;
    ctx.object_id.symbol_id = encode_symbol_id(sym.id);

    ctx.signature = sym.signature.empty() ? std::string(sym.symbol.name)
                                          : std::string(sym.signature);
    ctx.documentation = std::string(sym.doc_comment);
    ctx.location.file_id = sym.symbol.file_id;
    ctx.location.line = sym.symbol.line;
    ctx.location.column = sym.symbol.column;
    return true;
}

CodeObjectContext ContextLookupEngine::get_context(
    const CodeObjectID& object_id, bool& ok) const {
    ok = false;
    CodeObjectContext ctx;
    ctx.object_id = object_id;
    ctx.generated_at = now_rfc3339_utc();
    ctx.context_version = "1.1.0";

    LookupDiagnostics& diag = ctx.diagnostics;

    // RCU: pin the snapshot ONCE. Every fill reads from this frozen snapshot;
    // re-pinning per section could straddle a concurrent reindex generation.
    auto snap = indexer_.ref_tracker().pin();
    diag.symbol_index_ready = snap != nullptr;
    diag.ref_tracker_ready = snap != nullptr;
    diag.call_graph_populated = indexer_.ref_tracker().has_relationships();
    if (!diag.call_graph_populated) {
        diag.add_error({"CALL_GRAPH_EMPTY",
                        "call graph index is empty - relationships not indexed",
                        "relationships", /*fatal=*/false});
        diag.add_warning(
            "Call graph is empty - relationship queries will return no data. "
            "Ensure full indexing is complete.");
    }

    // Resolve the target symbol ONCE (trap 8: hoist target resolution instead
    // of rescanning per section). basic_info is FATAL on a miss.
    auto decoded = decode_symbol_id(object_id.symbol_id);
    ReferenceTracker::Snapshot::SymbolHandle sym;
    if (decoded && snap) sym = snap->get_enhanced_symbol(*decoded);
    if (sym == nullptr) {
        diag.add_error({"SYMBOL_NOT_FOUND",
                        "symbol not found in index", "object_id",
                        /*fatal=*/true});
        return ctx;  // ok stays false — fatal.
    }

    fill_basic_info(ctx, *sym);

    // Section fills run in order basic_info -> relationships -> variables ->
    // semantic -> structure -> ... Relationships (S3) lands independently;
    // the variables/semantic/structure sections do not depend on it.
    fill_direct_relationships(ctx, *snap, indexer_.ref_tracker(),
                              confidence_threshold());
    fill_variable_context(ctx, *snap);
    fill_semantic_context(ctx, *snap, indexer_, graph_propagator_,
                          semantic_annotator_);
    fill_structure_context(ctx, *snap, indexer_);
    fill_usage_analysis(ctx, *snap, indexer_);
    fill_ai_context(ctx, include_ai_text());

    ok = true;
    return ctx;
}

void ContextLookupEngine::sort_by_confidence_desc(
    std::vector<ObjectReference>& refs) {
    std::stable_sort(refs.begin(), refs.end(),
                     [](const ObjectReference& a, const ObjectReference& b) {
                         return a.confidence > b.confidence;
                     });
}

std::vector<ObjectReference> ContextLookupEngine::filter_high_confidence(
    const std::vector<ObjectReference>& refs, double threshold) {
    std::vector<ObjectReference> out;
    out.reserve(refs.size());
    for (const auto& ref : refs) {
        if (ref.confidence >= threshold) out.push_back(ref);
    }
    return out;
}

void ContextLookupEngine::filter_context_sections(
    CodeObjectContext& ctx,
    const std::vector<std::string>& include_sections,
    const std::vector<std::string>& exclude_sections) {
    // No section constraints → full context untouched (Go early return).
    if (include_sections.empty() && exclude_sections.empty()) return;

    // Exclude pass: zero each NAMED section. Unknown tokens match nothing and
    // fall through (Go's switch has no default).
    for (const auto& section : exclude_sections) {
        if (section == "relationships") {
            ctx.direct_relationships = DirectRelationships{};
        } else if (section == "variables") {
            ctx.variable_context = VariableContext{};
        } else if (section == "semantic") {
            ctx.semantic_context = SemanticContext{};
        } else if (section == "structure") {
            ctx.structure_context = StructureContext{};
        } else if (section == "usage") {
            ctx.usage_analysis = UsageAnalysis{};
        } else if (section == "ai") {
            ctx.ai_context = AIContext{};
        }
    }

    // Include pass: whitelist — zero every section NOT requested. Only runs
    // when include_sections is non-empty (Go gates on len > 0). An unknown
    // include token whitelists nothing (bug-for-bug parity): it sits in the
    // request set but no section is keyed on it, so listing only unknown
    // tokens zeroes all six.
    if (!include_sections.empty()) {
        auto requested = [&](const char* name) {
            for (const auto& s : include_sections) {
                if (s == name) return true;
            }
            return false;
        };
        if (!requested("relationships")) {
            ctx.direct_relationships = DirectRelationships{};
        }
        if (!requested("variables")) ctx.variable_context = VariableContext{};
        if (!requested("semantic")) ctx.semantic_context = SemanticContext{};
        if (!requested("structure")) ctx.structure_context = StructureContext{};
        if (!requested("usage")) ctx.usage_analysis = UsageAnalysis{};
        if (!requested("ai")) ctx.ai_context = AIContext{};
    }
}

void ContextLookupEngine::dedup_references(std::vector<ObjectReference>& refs) {
    absl::flat_hash_set<std::string> seen;
    seen.reserve(refs.size());
    std::vector<ObjectReference> out;
    out.reserve(refs.size());
    for (auto& ref : refs) {
        std::string key = ref.object_id.symbol_id + '\0' + ref.context;
        if (seen.insert(std::move(key)).second) out.push_back(std::move(ref));
    }
    refs = std::move(out);
}

int64_t ContextLookupEngine::per_component_time_ms(int64_t total_ms) {
    // RED stub: not yet divided across components.
    return total_ms;
}

}  // namespace lci
