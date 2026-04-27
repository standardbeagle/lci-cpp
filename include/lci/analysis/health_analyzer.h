#pragma once

#include <string>
#include <utility>
#include <vector>

#include <lci/analysis/codebase_intelligence_types.h>
#include <lci/symbol.h>

namespace lci {

/// Analyzes codebase health by computing complexity, hotspots, smells, and risk.
///
/// Operates on file and symbol data to produce health metrics.
/// Ported from Go: codebase_intelligence_health.go
class HealthAnalyzer {
  public:
    HealthAnalyzer() = default;

    /// Calculates complexity metrics from file data.
    ComplexityMetrics calculate_complexity_from_files(
        const std::vector<FileSymbolData>& files) const;

    /// Identifies code hotspots from file data.
    std::vector<Hotspot> identify_hotspots_from_files(
        const std::vector<FileSymbolData>& files) const;

    /// Calculates overall health score (0-10).
    static double calculate_overall_health_score(
        const ComplexityMetrics& complexity, int total_files);

    /// Calculates technical debt ratio from files.
    double calculate_tech_debt_ratio_from_files(
        const std::vector<FileSymbolData>& files) const;

    /// Estimates remediation time from debt ratio.
    static std::string estimate_debt_remediation_time(double ratio);

    /// Identifies components with high debt.
    std::vector<std::string> identify_debt_components(
        const std::vector<FileSymbolData>& files) const;

    /// Detects code smells with severity levels.
    std::vector<CodeSmellEntry> calculate_detailed_code_smells(
        const std::vector<FileSymbolData>& files) const;

    /// Finds symbols with quality issues above risk cutoff.
    std::vector<ProblematicSymbol> identify_problematic_symbols(
        const std::vector<FileSymbolData>& files) const;

    /// Calculates quality metrics from complexity.
    static QualityMetrics calculate_quality_from_complexity(
        const ComplexityMetrics& complexity);

    /// Gets maintainability rating letter grade.
    static std::string get_maintainability_rating(double score);

    /// Counts child methods of a class/struct within a file.
    static int count_child_methods(
        const std::vector<const EnhancedSymbol*>& symbols,
        const EnhancedSymbol& parent);

    /// Calculates risk score and quality tags for a symbol.
    static std::pair<std::vector<std::string>, int>
    calculate_symbol_risk_and_tags(const EnhancedSymbol& sym);

    /// Counts smells by type.
    static absl::flat_hash_map<std::string, int> count_smells_by_type(
        const std::vector<CodeSmellEntry>& smells);

    /// Returns numeric rank for severity comparison.
    static int severity_rank(std::string_view severity);

    /// Sorts smells by severity and limits to max_count.
    static std::vector<CodeSmellEntry> sort_and_limit_smells(
        std::vector<CodeSmellEntry> smells, int max_count);

    /// Sets exclusion patterns for test file filtering.
    void set_exclude_patterns(std::vector<std::string> patterns);

  private:
    std::vector<std::string> exclude_patterns_;

    bool is_excluded_file(std::string_view path) const;
    static bool is_test_helper_function(std::string_view name);
    static bool is_test_helper_path(std::string_view path);
};

}  // namespace lci
