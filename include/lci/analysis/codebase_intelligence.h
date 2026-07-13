#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/analysis/codebase_intelligence_types.h>
#include <lci/types.h>

namespace lci {

// ============================================================================
// Parameters (matching Go CodebaseIntelligenceParams)
// ============================================================================

/// Include flags for overview mode.
struct IncludeFlags {
    bool repository_map{};
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
    ///
    /// Runs the module/layer/feature/vocabulary analyzers and populates the
    /// matching response field (module_analysis / layer_analysis /
    /// feature_analysis / domain_terms). `project_root` relativizes module
    /// paths; `callees_of` supplies the reference-graph edges for feature
    /// clustering (typically ref_tracker.get_callee_symbols). Both are
    /// required: they come from a live index, so this builder is only reachable
    /// through the index-backed caller (the MCP handler), never through the
    /// index-less analyze() dispatch. Passing an empty `callees_of` would
    /// silently skip feature clustering, so it is not defaulted.
    CodebaseIntelligenceResponse build_detailed(
        const CodebaseIntelligenceParams& params,
        const std::vector<FileSymbolData>& files,
        std::string_view project_root,
        const std::function<std::vector<SymbolID>(SymbolID)>& callees_of)
        const;

    /// Builds statistics analysis (Tier 3). Runs the coupling analyzer and
    /// derives quality from complexity, populating statistics_report.
    /// `project_root` relativizes module paths and `purity_ratio` is supplied
    /// by the caller (from the side-effect analyzer, which the engine does not
    /// own). Both are required — they come from the index-backed caller, so
    /// this builder is not reachable through the index-less analyze() dispatch.
    CodebaseIntelligenceResponse build_statistics(
        const CodebaseIntelligenceParams& params,
        const std::vector<FileSymbolData>& files,
        std::string_view project_root,
        double purity_ratio) const;

    /// Builds unified analysis (all tiers combined).
    CodebaseIntelligenceResponse build_unified(
        const CodebaseIntelligenceParams& params,
        const std::vector<FileSymbolData>& files,
        int file_count, int symbol_count) const;

    /// Builds structure analysis (directory tree exploration). `file_paths`
    /// is the full set of indexed file paths (relativized against
    /// `project_root`); `file_count` and `total_functions` populate the
    /// summary line. Populates structure_analysis. All inputs are required —
    /// they come from a live index, so this builder is only reachable through
    /// the index-backed caller (the MCP handler); an empty `file_paths` would
    /// yield an empty tree, so it is not defaulted.
    CodebaseIntelligenceResponse build_structure(
        const CodebaseIntelligenceParams& params,
        const std::vector<FileSymbolData>& files,
        const std::vector<std::string>& file_paths,
        std::string_view project_root, int file_count,
        int total_functions) const;


  private:
    /// Extracts critical functions sorted by importance.
    std::vector<FunctionSignature> extract_critical_functions(
        const std::vector<FileSymbolData>& files, int max_results) const;

    /// Builds entry points list from files.
    EntryPointsList build_entry_points(
        const std::vector<FileSymbolData>& files) const;

    /// Applies default values to params.
    static CodebaseIntelligenceParams apply_defaults(
        CodebaseIntelligenceParams params);
};

}  // namespace lci
