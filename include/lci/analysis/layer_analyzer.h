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

    /// Runs layer analysis on the given file/symbol data. `project_root`
    /// scopes module (package) naming, same as ModuleAnalyzer.
    LayerAnalysis analyze(const std::vector<FileSymbolData>& files,
                          std::string_view project_root) const;

    /// Classifies a single symbol to a layer name.
    static std::string classify_symbol_to_layer(const EnhancedSymbol& sym);

    /// Detects architectural patterns from the layer set. Confidence is the
    /// measured share of symbols in the pattern's layers; low-coverage
    /// patterns are not reported.
    static std::vector<LayerPattern> detect_patterns(
        const std::vector<ArchitecturalLayer>& layers);
};

}  // namespace lci
