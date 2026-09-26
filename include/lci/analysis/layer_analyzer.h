#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <lci/analysis/codebase_intelligence_types.h>
#include <lci/symbol.h>

namespace lci {

/// Classifies symbols into architectural layers and detects patterns.
///
/// Ported from Go: layer_analysis.go
class LayerAnalyzer {
  public:
    LayerAnalyzer() = default;

    /// The layer reported for symbols the keyword table cannot classify.
    /// Reported as its own bucket, never defaulted into Utility — doing so made
    /// every other layer read modules=0 on repos whose symbols are mostly
    /// ordinary member functions (lci itself).
    static constexpr std::string_view kUnclassified = "unclassified";

    /// The one declared dependency order, shallowest first. Depth is the rank
    /// in THIS list (Presentation 0 ... Infrastructure 4); see depth_of. The
    /// violation check and the emitted `depth=` field both consume it, so the
    /// two can never disagree again (ANA-5).
    static const std::vector<std::string_view>& layer_order();

    /// Rank of `layer_name` in layer_order(), or -1 for cross-cutting /
    /// unknown layers (Utility Layer, unclassified) that are exempt from the
    /// downward-flow check.
    static int depth_of(std::string_view layer_name);

    /// Runs layer analysis on the given file/symbol data. `project_root`
    /// scopes module (package) naming, same as ModuleAnalyzer.
    LayerAnalysis analyze(const std::vector<FileSymbolData>& files,
                          std::string_view project_root) const;

    /// Classifies a single symbol to a layer name, or kUnclassified when no
    /// keyword matches.
    static std::string classify_symbol_to_layer(const EnhancedSymbol& sym);

    /// Detects architectural patterns from the layer set. Confidence is the
    /// measured share of symbols in the pattern's layers; low-coverage
    /// patterns are not reported.
    static std::vector<LayerPattern> detect_patterns(
        const std::vector<ArchitecturalLayer>& layers);
};

}  // namespace lci
