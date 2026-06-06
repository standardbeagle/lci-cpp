#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/analysis/codebase_intelligence_types.h>

namespace lci {

// ============================================================================
// Parameters (matching Go CodebaseIntelligenceParams)
// ============================================================================

/// Include flags for overview mode.
struct IncludeFlags {
    bool repository_map{};
    bool dependency_graph{};
    bool health_dashboard{};
    bool entry_points{};
};

/// Parameters for codebase intelligence analysis.
struct CodebaseIntelligenceParams {
    std::string mode;
    std::optional<int> tier;
    IncludeFlags include;
    std::optional<std::string> analysis;
    std::optional<std::vector<std::string>> metrics;
    std::optional<std::string> granularity;
    std::optional<int> max_results;
    std::optional<double> confidence_threshold;
    std::optional<std::string> domain;
    std::optional<std::string> query;
    std::optional<std::string> focus;
    std::optional<std::string> target;
    std::vector<std::string> languages;
};

// ============================================================================
// Constants
// ============================================================================

namespace ci_defaults {
inline constexpr int kDefaultTier = 1;
inline constexpr int kDefaultMaxResults = 50;
inline constexpr double kDefaultConfidenceThreshold = 0.0;
inline constexpr const char* kDefaultGranularity = "module";
inline constexpr double kImportanceThresholdMetrics = 20.0;
inline constexpr double kImportanceThresholdEnhanced = 25.0;
}  // namespace ci_defaults

// ============================================================================
// Engine
// ============================================================================

/// Codebase intelligence engine providing 7 analysis modes.
///
/// Operates on pre-collected file/symbol data rather than a live index.
/// The caller (MCP layer) is responsible for gathering FileSymbolData.
///
/// Ported from Go: codebase_intelligence_tools.go (mode dispatch + builders)
class CodebaseIntelligenceEngine {
  public:
    CodebaseIntelligenceEngine() = default;

    /// Validates a mode string. Returns true if the mode is recognized.
    static bool is_valid_mode(std::string_view mode);

    /// Runs the full analysis pipeline: validate, dispatch, metadata, budget.
    /// Returns the response on success, or an error string on failure.
    struct Result {
        CodebaseIntelligenceResponse response;
        std::string error;
        bool ok() const { return error.empty(); }
    };

    Result analyze(const CodebaseIntelligenceParams& params,
                   const std::vector<FileSymbolData>& files,
                   int file_count, int symbol_count) const;

    /// Calculates importance score for a symbol (used for ranking).
    static double calculate_importance_score(const EnhancedSymbol& sym);

    /// Builds overview analysis (Tier 1).
    CodebaseIntelligenceResponse build_overview(
        const CodebaseIntelligenceParams& params,
        const std::vector<FileSymbolData>& files,
        int file_count, int symbol_count) const;

    /// Builds detailed analysis (Tier 2) - dispatches by analysis type.
    CodebaseIntelligenceResponse build_detailed(
        const CodebaseIntelligenceParams& params,
        const std::vector<FileSymbolData>& files) const;

    /// Builds statistics analysis (Tier 3).
    CodebaseIntelligenceResponse build_statistics(
        const CodebaseIntelligenceParams& params,
        const std::vector<FileSymbolData>& files) const;

    /// Builds unified analysis (all tiers combined).
    CodebaseIntelligenceResponse build_unified(
        const CodebaseIntelligenceParams& params,
        const std::vector<FileSymbolData>& files,
        int file_count, int symbol_count) const;

    /// Builds structure analysis (directory tree exploration).
    CodebaseIntelligenceResponse build_structure(
        const CodebaseIntelligenceParams& params,
        const std::vector<FileSymbolData>& files) const;


  private:
    /// Extracts critical functions sorted by importance.
    std::vector<FunctionSignature> extract_critical_functions(
        const std::vector<FileSymbolData>& files, int max_results) const;

    /// Builds dependency graph nodes from symbols.
    DependencyGraph build_dependency_graph(
        const std::vector<FileSymbolData>& files) const;

    /// Builds entry points list from files.
    EntryPointsList build_entry_points(
        const std::vector<FileSymbolData>& files) const;

    /// Applies default values to params.
    static CodebaseIntelligenceParams apply_defaults(
        CodebaseIntelligenceParams params);
};

}  // namespace lci
