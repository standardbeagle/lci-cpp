#pragma once

#include <functional>
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

    /// Graph-based module detection: modules ARE the Louvain communities of
    /// the call graph; directories are their labels (majority directory of
    /// the members, ties lexicographic). Per-module cohesion = share of the
    /// community's incident call edges that stay internal; coupling = the
    /// external share; external_deps = distinct other modules it has edges
    /// to; stability = Martin instability (efferent / (afferent + efferent)
    /// module edges). Divergence between layout and structure is surfaced,
    /// not hidden: spanned_dirs/dir_purity per module and split_dirs for
    /// directories whose symbols land in several communities. Falls back to
    /// analyze() (directory grouping) when `callees_of` is null. Communities
    /// smaller than 2 symbols are not modules and are dropped. Deterministic:
    /// members and modules sorted before emission.
    ModuleAnalysis analyze_graph(
        const std::vector<FileSymbolData>& files,
        std::string_view project_root,
        const std::function<std::vector<SymbolID>(SymbolID)>& callees_of) const;

    /// Classifies a module type based on its directory path.
    static std::string classify_module_by_path(std::string_view path);
};

}  // namespace lci
