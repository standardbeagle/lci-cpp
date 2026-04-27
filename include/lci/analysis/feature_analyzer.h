#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <lci/analysis/codebase_intelligence_types.h>
#include <lci/symbol.h>

namespace lci {

/// Identifies features in a codebase by matching symbol names and
/// directory structure against known feature patterns.
///
/// Ported from Go: feature_analysis.go
class FeatureAnalyzer {
  public:
    FeatureAnalyzer() = default;

    /// Runs feature analysis on the given file/symbol data.
    FeatureAnalysis analyze(const std::vector<FileSymbolData>& files) const;

    /// Classifies a component type based on symbol name and type.
    static std::string classify_component_type(const EnhancedSymbol& sym);

    /// Classifies a feature type by its pattern name.
    static std::string classify_feature_type(std::string_view name);
};

}  // namespace lci
