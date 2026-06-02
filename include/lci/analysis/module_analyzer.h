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

    /// Runs module analysis on the given file/symbol data. Module names and
    /// type classification both use the package directory relative to
    /// `project_root` ("(root)" for root-level files), consistent with the
    /// repository-map builder. (Go's ModuleAnalysis builder names by basename
    /// and classifies on the absolute path — a self-inconsistent bug the C++
    /// port does not replicate.) Non-code files are skipped and
    /// `function_count` counts only functions/methods. Modules are returned
    /// sorted by file_count descending, name ascending (deterministic; Go
    /// relies on map order for ties).
    ModuleAnalysis analyze(const std::vector<FileSymbolData>& files,
                           std::string_view project_root = {}) const;

    /// Classifies a module type based on its directory path.
    static std::string classify_module_by_path(std::string_view path);
};

}  // namespace lci
