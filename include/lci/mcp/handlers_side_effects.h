#pragma once

#include <nlohmann/json.hpp>

#include <lci/mcp/server.h>

namespace lci {

class MasterIndex;
class SemanticAnnotator;
class SideEffectAnalyzer;
class GraphPropagator;

namespace mcp {

/// Handles "semantic_annotations": queries symbols by @lci: labels/categories.
ToolResult handle_semantic_annotations(const nlohmann::json& params,
                                       SemanticAnnotator& annotator,
                                       GraphPropagator* propagator,
                                       MasterIndex* indexer = nullptr);

/// Handles "side_effects": queries function purity with 6 modes.
ToolResult handle_side_effects(const nlohmann::json& params,
                               SideEffectAnalyzer& analyzer,
                               MasterIndex* indexer);

}  // namespace mcp
}  // namespace lci
