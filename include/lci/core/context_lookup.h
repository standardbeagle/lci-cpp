#pragma once

#include <atomic>
#include <vector>

#include <lci/core/context_lookup_types.h>

namespace lci {

class MasterIndex;
struct EnhancedSymbol;
class GraphPropagator;
class SemanticAnnotator;

// ContextLookupEngine — C++ port of internal/core/context_lookup.go.
//
// S1 skeleton: pins the ReferenceTracker snapshot ONCE per get_context and
// passes it through every fill (RCU discipline — never re-snapshot per
// section). fill_basic_info is FATAL on a missing symbol; every other section
// soft-fails (records a diagnostic and leaves the section empty-but-present).
// Later slices (S3-S8) populate the remaining sections in place.
class ContextLookupEngine {
  public:
    explicit ContextLookupEngine(MasterIndex& indexer) : indexer_(indexer) {}

    // -- Config (atomic, lock-free) -----------------------------------------
    // Trap 8: the confidence threshold is a std::atomic<double>, not the Go
    // math.Float64bits-in-an-int64 dance.

    void set_max_context_depth(int depth) {
        if (depth > 0 && depth <= 20) {
            max_context_depth_.store(depth, std::memory_order_relaxed);
        }
    }
    int max_context_depth() const {
        return max_context_depth_.load(std::memory_order_relaxed);
    }

    void set_include_ai_text(bool include) {
        include_ai_text_.store(include, std::memory_order_relaxed);
    }
    bool include_ai_text() const {
        return include_ai_text_.load(std::memory_order_relaxed);
    }

    void set_confidence_threshold(double threshold) {
        if (threshold >= 0.0 && threshold <= 1.0) {
            confidence_threshold_.store(threshold, std::memory_order_relaxed);
        }
    }
    double confidence_threshold() const {
        return confidence_threshold_.load(std::memory_order_relaxed);
    }

    // GraphPropagator/SemanticAnnotator (CLX S6, semantic section) are
    // OPTIONAL collaborators: a caller that hasn't built one for this index
    // simply never wires it, and fill_semantic_context degrades to an
    // empty/default result for the parts that need it rather than crashing
    // (mirrors Go's `if cle.graphPropagator != nil` / semanticAnnotator==nil
    // gates). Plain non-owning pointers — lifetime is the caller's.
    void set_graph_propagator(GraphPropagator* propagator) {
        graph_propagator_ = propagator;
    }
    GraphPropagator* graph_propagator() const { return graph_propagator_; }

    void set_semantic_annotator(SemanticAnnotator* annotator) {
        semantic_annotator_ = annotator;
    }
    SemanticAnnotator* semantic_annotator() const {
        return semantic_annotator_;
    }

    // Builds the full context for `object_id`. Pins the tracker snapshot once
    // and resolves the target symbol once (trap 8: hoist target resolution),
    // then fills each section. `ok` is set false when a FATAL section
    // (basic_info) fails — the returned context still carries diagnostics.
    CodeObjectContext get_context(const CodeObjectID& object_id,
                                  bool& ok) const;

    // -- Reference post-processing helpers (trap 8) -------------------------
    // std::stable_sort DESC by confidence — NOT Go's O(n^2) bubble sort.

    static void sort_by_confidence_desc(std::vector<ObjectReference>& refs);

    // Keeps only references at or above `threshold`.
    static std::vector<ObjectReference> filter_high_confidence(
        const std::vector<ObjectReference>& refs, double threshold);

    // Removes duplicate references (same symbol_id + context), keeping the
    // first (highest-confidence after a sort) occurrence.
    static void dedup_references(std::vector<ObjectReference>& refs);

    // Zeroes CodeObjectContext sections per include/exclude lists — C++ port
    // of Go's Server.filterContextSections (internal/mcp/handlers.go:2578).
    // The exclude pass zeroes each NAMED section; the include pass (only when
    // include_sections is non-empty) whitelists — zeroing every section NOT
    // named. Both passes ONLY zero (default-construct) a section, never
    // restore. Filtered sections stay PRESENT in to_json with empty/zero
    // values — keys are never omitted. The six tokens are exactly
    // relationships, variables, semantic, structure, usage, ai; unknown tokens
    // are silently ignored (bug-for-bug parity: an unknown include token
    // whitelists nothing). No include and no exclude is a no-op.
    static void filter_context_sections(
        CodeObjectContext& ctx,
        const std::vector<std::string>& include_sections,
        const std::vector<std::string>& exclude_sections);

  private:
    // Fills identification/signature/location from the resolved symbol.
    // Returns false only if the symbol is unusable (fatal).
    bool fill_basic_info(CodeObjectContext& ctx,
                         const EnhancedSymbol& sym) const;

    MasterIndex& indexer_;
    std::atomic<int> max_context_depth_{5};
    std::atomic<bool> include_ai_text_{true};
    std::atomic<double> confidence_threshold_{0.3};
    GraphPropagator* graph_propagator_{nullptr};
    SemanticAnnotator* semantic_annotator_{nullptr};
};

}  // namespace lci
