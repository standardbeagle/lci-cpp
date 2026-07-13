#pragma once

#include <atomic>
#include <vector>

#include <lci/core/context_lookup_types.h>

namespace lci {

class MasterIndex;
struct EnhancedSymbol;

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

  private:
    // Fills identification/signature/location from the resolved symbol.
    // Returns false only if the symbol is unusable (fatal).
    bool fill_basic_info(CodeObjectContext& ctx,
                         const EnhancedSymbol& sym) const;

    MasterIndex& indexer_;
    std::atomic<int> max_context_depth_{5};
    std::atomic<bool> include_ai_text_{true};
    std::atomic<double> confidence_threshold_{0.3};
};

}  // namespace lci
