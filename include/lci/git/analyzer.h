#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <nlohmann/json_fwd.hpp>

#include <lci/analysis/code_similarity.h>
#include <lci/analysis/naming_analyzer.h>
#include <lci/git/provider.h>
#include <lci/git/types.h>
#include <lci/indexing/master_index.h>

namespace lci {
namespace git {

// ============================================================================
// Free utility functions (testable)
// ============================================================================

// normalize_code_content / code_token_set / token_set_similarity /
// code_structural_similarity moved to lci/analysis/code_similarity.h so the
// corpus-wide clone detector shares them without depending on git headers;
// unqualified callers in lci::git still resolve them via the parent
// namespace.

/// Filters a corpus-wide NamingReport down to findings involving the changed
/// symbols (matched by name). This is the git naming focus: the report-side
/// NamingAnalyzer signals (synonym splits, ambiguous names, vague names,
/// vocabulary outliers) replace the bespoke fuzzy similar-name and
/// abbreviation-table checks, which overlapped them at lower precision.
void naming_findings_from_report(const NamingReport& report,
                                 const std::vector<const SymbolInfo*>& changed,
                                 std::vector<NamingFinding>& out);

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

/// Lexically normalizes an absolute path to relative-to-project-root (the
/// serializer's canonical path form). Relative paths pass through with
/// separators normalized to '/'. Exposed so the analyzer can compare index
/// paths (absolute) against git diff paths (repo-relative) in one form.
std::string normalize_rel(const std::string& p,
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

    /// Extracts symbols from every readable changed file. `skipped_out`
    /// receives the count of files whose content could not be read.
    bool parse_changed_files(const std::vector<ChangedFile>& files,
                             const AnalysisParams& params,
                             std::vector<SymbolInfo>& out,
                             int& skipped_out);

    /// Snapshots the indexed function/method symbols. `with_content` gates
    /// the per-symbol body extraction — only the duplicate finder reads
    /// content, and copying every function body on every request was the
    /// dominant cost when duplicates were not in focus.
    void get_existing_symbols(bool with_content, std::vector<SymbolInfo>& out);

    void find_duplicates(const std::vector<SymbolInfo>& new_symbols,
                         const std::vector<SymbolInfo>& existing_symbols,
                         const AnalysisParams& params,
                         std::vector<DuplicateFinding>& out);

    void check_naming(const std::vector<SymbolInfo>& new_symbols,
                      const AnalysisParams& params,
                      std::vector<NamingFinding>& out);

    bool check_case_style(const SymbolInfo& sym, NamingFinding& out);

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
                      int64_t elapsed_ms, int skipped_unreadable,
                      AnalysisReport& out);

    void empty_report(const AnalysisParams& params, int64_t elapsed_ms,
                      AnalysisReport& out);
};

}  // namespace git
}  // namespace lci
