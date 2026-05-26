#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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

// -- Handler functions (exposed for testing) ----------------------------------

/// Handles "semantic_annotations": queries symbols by @lci: labels/categories.
ToolResult handle_semantic_annotations(const nlohmann::json& params,
                                       SemanticAnnotator& annotator,
                                       GraphPropagator* propagator,
                                       MasterIndex* indexer = nullptr);

/// Handles "side_effects": queries function purity with 6 modes.
ToolResult handle_side_effects(const nlohmann::json& params,
                               SideEffectAnalyzer& analyzer,
                               MasterIndex* indexer);

/// Handles "code_insight": dispatches to CodebaseIntelligenceEngine.
/// When `analyzer` is non-null, unified mode reads per-function purity from
/// it to populate the HEALTH section's purity total/pure/impure counters.
/// When null (legacy callers), purity reports total=N pure=0 impure=0.
ToolResult handle_code_insight(const nlohmann::json& params,
                               CodebaseIntelligenceEngine& engine,
                               MasterIndex& indexer,
                               SideEffectAnalyzer* analyzer = nullptr);

}  // namespace mcp
}  // namespace lci
