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

    /// Runs layer analysis on the given file/symbol data.
    LayerAnalysis analyze(const std::vector<FileSymbolData>& files) const;

    /// Classifies a single symbol to a layer name.
    static std::string classify_symbol_to_layer(const EnhancedSymbol& sym);

    /// Detects architectural patterns from the layer set.
    static std::vector<LayerPattern> detect_patterns(
        const std::vector<ArchitecturalLayer>& layers);
};

}  // namespace lci
