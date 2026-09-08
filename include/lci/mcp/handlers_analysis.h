#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <nlohmann/json.hpp>

#include <lci/core/reference_tracker.h>
#include <lci/mcp/server.h>

namespace lci {

class MasterIndex;
class SemanticAnnotator;
class SideEffectAnalyzer;
class GraphPropagator;
class CodebaseIntelligenceEngine;

namespace mcp {

/// Registers the 3 analysis tool handlers (semantic_annotations, side_effects,
/// code_insight) on the given server, replacing stub handlers.
///
/// Any pointer may be null; handlers will return errors when their backing
/// component is unavailable.
void register_analysis_handlers(McpServer& server,
                                MasterIndex* indexer,
                                SemanticAnnotator* annotator,
                                SideEffectAnalyzer* analyzer,
                                GraphPropagator* propagator,
                                CodebaseIntelligenceEngine* ci_engine);

/// BETA error-report capture (insight.error_report = "capture"): renders the
/// full, untruncated == ERROR HANDLING == / == RESOURCE MANAGEMENT ==
/// sections and writes them atomically (tmp + rename) to
/// $XDG_STATE_HOME/lci/error-reports/<root-slug>.txt (fallback
/// ~/.local/state). Generate-don't-publish: nothing appears in any tool
/// response. Called from the MCP session teardown after the transport
/// exits — the index is complete and the cost is off every request path.
/// No-op in modes "off" and "on" ("on" publishes in-band instead). Returns
/// the written path, or "" when nothing was written (an error is printed
/// to stderr on failure — never silent).
std::string write_error_report_capture(MasterIndex& indexer,
                                       SideEffectAnalyzer* analyzer);

// -- Same-name call grouping --------------------------------------------------

/// Precomputed name -> {dynamic, unresolved} call-site split over one
/// ReferenceTracker snapshot. Replaces per-candidate
/// Snapshot::classify_same_name_calls calls (each a full scan of every
/// reference) with ONE pass over the snapshot plus O(1) lookups — unified and
/// deadcode modes classify per zero-in-degree candidate, which was
/// O(candidates x refs) on large corpora.
///
/// Keys are the tail of each interned call-site spelling (text after the
/// last '.'), which reproduces classify_same_name_calls' matching rule for
/// bare-name queries: spelling == name, or a qualified "Recv.name" spelling.
class SameNameCallGrouping {
  public:
    using Stats = ReferenceTracker::Snapshot::SameNameCallStats;

    /// One pass over the snapshot's live Call references.
    static SameNameCallGrouping build(const ReferenceTracker::Snapshot& snap);

    /// The split for a bare symbol name; zeros when no same-name call site
    /// exists.
    Stats lookup(std::string_view name) const {
        auto it = by_tail_.find(name);
        return it == by_tail_.end() ? Stats{} : it->second;
    }

  private:
    absl::flat_hash_map<std::string, Stats> by_tail_;
};

// -- Handler functions (exposed for testing) ----------------------------------
// semantic_annotations / side_effects live in handlers_side_effects.h.

/// Handles "code_insight": dispatches to CodebaseIntelligenceEngine.
/// When `analyzer` is non-null, unified mode reads per-function purity from
/// it to populate the HEALTH section's purity total/pure/impure counters.
/// When null (legacy callers), purity reports total=N pure=0 impure=0.
/// When `propagator` is non-null, the == CLUSTERS == section cross-references
/// Louvain communities with propagated @lci: labels to name domains.
/// `sem_annotator`, when non-null, lets ENTRY POINTS honor @lci:entry
/// annotations (tier 2 of analysis::resolve_entry_hints).
ToolResult handle_code_insight(const nlohmann::json& params,
                               CodebaseIntelligenceEngine& engine,
                               MasterIndex& indexer,
                               SideEffectAnalyzer* analyzer = nullptr,
                               GraphPropagator* propagator = nullptr,
                               SemanticAnnotator* sem_annotator = nullptr);

}  // namespace mcp
}  // namespace lci
