#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <lci/analysis/codebase_intelligence_types.h>
#include <lci/symbol.h>

namespace lci {

/// Detects module boundaries by grouping symbols into directories
/// and computing per-module cohesion, coupling, and stability.
///
/// Ported from Go: module_analysis.go
class ModuleAnalyzer {
  public:
    ModuleAnalyzer() = default;

    /// Runs module analysis on the given file/symbol data.
    ModuleAnalysis analyze(const std::vector<FileSymbolData>& files) const;

    /// Classifies a module type based on its directory path.
    static std::string classify_module_by_path(std::string_view path);
};

}  // namespace lci
