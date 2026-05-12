#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <lci/git/provider.h>
#include <lci/git/types.h>
#include <lci/indexing/master_index.h>
#include <lci/semantic/fuzzy_matcher.h>
#include <lci/semantic/name_splitter.h>

namespace lci {
namespace git {

// ============================================================================
// Free utility functions (testable)
// ============================================================================

/// Strips comments and blank lines for content comparison.
std::string normalize_code_content(std::string_view content);

/// Computes Jaccard token-similarity between two code blocks.
double code_structural_similarity(std::string_view a, std::string_view b);

/// Extracts the source text for a symbol from file content (1-based lines).
std::string extract_symbol_content(std::string_view content,
                                   int start_line, int end_line);

/// Checks if a file has a supported source extension for analysis.
bool is_analysis_supported_file(std::string_view path);

/// Serializes an AnalysisReport to the canonical JSON shape used by the
/// HTTP /git-analyze endpoint and the MCP git_analysis tool. All
/// `file_path` values are normalized to relative-to-project-root so the
/// caller doesn't see a mix of absolute paths (from the index) and
/// relative paths (from the git changed-files iterator).
nlohmann::json report_to_json(const AnalysisReport& report,
                              const std::string& project_root);

// ============================================================================
// Analyzer
// ============================================================================

/// Performs git change analysis comparing new code against an existing index.
/// Ported from Go: internal/git/analyzer.go
class Analyzer {
  public:
    Analyzer(Provider& provider, MasterIndex& index);

    /// Runs complete change analysis for the given parameters.
    /// Returns true on success and fills `out` with the report.
    bool analyze(const AnalysisParams& params, AnalysisReport& out);

  private:
    Provider& provider_;
    MasterIndex& index_;
    FuzzyMatcher fuzzy_matcher_;
    NameSplitter name_splitter_;

    bool parse_changed_files(const std::vector<ChangedFile>& files,
                             const AnalysisParams& params,
                             std::vector<SymbolInfo>& out);

    void get_existing_symbols(std::vector<SymbolInfo>& out);

    void find_duplicates(const std::vector<SymbolInfo>& new_symbols,
                         const std::vector<SymbolInfo>& existing_symbols,
                         const AnalysisParams& params,
                         std::vector<DuplicateFinding>& out);

    void check_naming(const std::vector<SymbolInfo>& new_symbols,
                      const std::vector<SymbolInfo>& existing_symbols,
                      const AnalysisParams& params,
                      std::vector<NamingFinding>& out);

    bool check_case_style(const SymbolInfo& sym, NamingFinding& out);

    bool find_similar_names(const SymbolInfo& sym,
                            const std::vector<SymbolInfo>& existing,
                            double threshold, NamingFinding& out);

    bool check_abbreviations(const SymbolInfo& sym,
                             const std::vector<SymbolInfo>& existing,
                             NamingFinding& out);

    void check_metrics(const std::vector<SymbolInfo>& new_symbols,
                       const std::vector<SymbolInfo>& existing_symbols,
                       const AnalysisParams& params,
                       std::vector<MetricsFinding>& out);

    void build_report(const std::vector<ChangedFile>& files,
                      const std::vector<SymbolInfo>& symbols,
                      std::vector<DuplicateFinding>& duplicates,
                      std::vector<NamingFinding>& naming_issues,
                      std::vector<MetricsFinding>& metrics_issues,
                      const AnalysisParams& params,
                      int64_t elapsed_ms, AnalysisReport& out);

    void empty_report(const AnalysisParams& params, int64_t elapsed_ms,
                      AnalysisReport& out);
};

}  // namespace git
}  // namespace lci
