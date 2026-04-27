#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <lci/analysis/codebase_intelligence_types.h>
#include <lci/symbol.h>

namespace lci {

/// Computes coupling and cohesion metrics at the package level.
///
/// Groups files by directory, counts inter-package references, and
/// calculates afferent/efferent coupling, instability, and cohesion.
/// Ported from Go: codebase_intelligence_coupling.go
class CouplingAnalyzer {
  public:
    CouplingAnalyzer() = default;

    /// Returns true if the file extension indicates source code.
    static bool is_code_file(std::string_view path);

    /// Extracts the package (directory) name relative to root.
    static std::string get_package_name(std::string_view file_path,
                                        std::string_view project_root);

    /// Computes coupling and cohesion metrics from file/symbol data.
    struct CouplingResult {
        CouplingMetrics coupling;
        CohesionMetrics cohesion;
    };

    CouplingResult analyze(const std::vector<FileSymbolData>& files,
                           std::string_view project_root) const;

    /// Sets exclusion patterns for filtering test packages.
    void set_exclude_patterns(std::vector<std::string> patterns);

  private:
    std::vector<std::string> exclude_patterns_;

    bool is_excluded(std::string_view name) const;
};

}  // namespace lci
