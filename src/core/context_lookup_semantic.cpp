// C++ port of the semantic section of internal/core/context_lookup.go
// (context_lookup_semantic.go: fillSemanticContext). RED stub: leaves
// ctx.semantic_context untouched so CLX S6 tests fail before the real
// implementation lands (red-green TDD).

#include <lci/core/context_lookup.h>

#include <lci/core/reference_tracker.h>

namespace lci {

void fill_semantic_context(CodeObjectContext& /*ctx*/,
                           const ReferenceTracker::Snapshot& /*snap*/,
                           MasterIndex& /*indexer*/,
                           GraphPropagator* /*propagator*/,
                           SemanticAnnotator* /*annotator*/) {
    // TODO(clx-port S6): implement per context_lookup_semantic.go.
}

}  // namespace lci
