#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <lci/analysis/codebase_intelligence_types.h>
#include <lci/symbol.h>
#include <lci/types.h>

namespace lci {

/// Identifies features in a codebase as communities in the symbol reference
/// graph, found via Louvain modularity maximization (analysis::CallGraph).
/// Each community is a feature; cohesion, complexity, confidence, and
/// cross-feature dependencies are derived from real graph structure (intra- vs
/// inter-community edges and per-symbol cyclomatic complexity) rather than
/// name-prefix keyword heuristics. Symbols Louvain leaves in a singleton
/// community (typically those with no call edges) cannot be graph-clustered
/// and are reported as orphan_components.
class FeatureAnalyzer {
  public:
    FeatureAnalyzer() = default;

    /// Runs feature analysis. `callees_of(id)` yields the callee SymbolIDs of a
    /// symbol — typically `ref_tracker.get_callee_symbols` — and defines the
    /// edges of the reference graph clustered into features. Edges to symbols
    /// outside the analyzed set are dropped by CallGraph::build.
    FeatureAnalysis analyze(
        const std::vector<FileSymbolData>& files,
        const std::function<std::vector<SymbolID>(SymbolID)>& callees_of) const;

    /// Classifies a component type based on symbol name and type.
    static std::string classify_component_type(const EnhancedSymbol& sym);

    /// Classifies a feature type by its pattern name.
    static std::string classify_feature_type(std::string_view name);
};

}  // namespace lci
