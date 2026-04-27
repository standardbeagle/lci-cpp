#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace lci {
namespace git {

// ============================================================================
// Analysis Scope & Parameters
// ============================================================================

/// Defines what changes to analyze.
enum class AnalysisScope : uint8_t {
    Staged,   ///< Only staged changes (git diff --cached)
    WIP,      ///< All uncommitted changes (staged + unstaged)
    Commit,   ///< A specific commit vs its parent
    Range,    ///< A commit range (base..target)
};

/// Returns the string name for an AnalysisScope value.
constexpr std::string_view to_string(AnalysisScope s) {
    switch (s) {
        case AnalysisScope::Staged: return "staged";
        case AnalysisScope::WIP: return "wip";
        case AnalysisScope::Commit: return "commit";
        case AnalysisScope::Range: return "range";
    }
    return "unknown";
}

/// Configures the git change analysis.
struct AnalysisParams {
    AnalysisScope scope{AnalysisScope::Staged};
    std::string base_ref;
    std::string target_ref;
    std::vector<std::string> focus;
    double similarity_threshold{0.8};
    int max_findings{20};

    /// Returns default analysis parameters.
    static AnalysisParams defaults() {
        return {AnalysisScope::Staged, {}, {}, {"duplicates", "naming", "metrics"}, 0.8, 20};
    }

    /// Checks if a specific focus area is enabled.
    bool has_focus(std::string_view f) const {
        if (focus.empty()) return true;
        for (const auto& item : focus) {
            if (item == f || item == "all") return true;
        }
        return false;
    }
};

// ============================================================================
// File Change Types
// ============================================================================

/// Indicates the type of change to a file.
enum class FileChangeStatus : uint8_t {
    Added,
    Modified,
    Deleted,
    Renamed,
    Copied,
};

/// Returns the string name for a FileChangeStatus value.
constexpr std::string_view to_string(FileChangeStatus s) {
    switch (s) {
        case FileChangeStatus::Added: return "added";
        case FileChangeStatus::Modified: return "modified";
        case FileChangeStatus::Deleted: return "deleted";
        case FileChangeStatus::Renamed: return "renamed";
        case FileChangeStatus::Copied: return "copied";
    }
    return "unknown";
}

/// Represents a file affected by git changes.
struct ChangedFile {
    std::string path;
    std::string old_path;
    FileChangeStatus status{FileChangeStatus::Modified};
    int lines_added{};
    int lines_deleted{};
};

/// Summary statistics for a diff.
struct DiffStats {
    int files_added{};
    int files_modified{};
    int files_deleted{};
    int files_renamed{};
    int total_added{};
    int total_deleted{};
};

/// Categorizes the size of a diff.
enum class DiffSize : uint8_t {
    Small,   ///< < 10 files
    Medium,  ///< 10-50 files
    Large,   ///< > 50 files
};

/// Determines the diff size category from a file count.
inline DiffSize categorize_diff_size(int file_count) {
    if (file_count < 10) return DiffSize::Small;
    if (file_count <= 50) return DiffSize::Medium;
    return DiffSize::Large;
}

// ============================================================================
// Symbol Information (for git analysis)
// ============================================================================

/// Provides details about a symbol for analysis.
struct SymbolInfo {
    std::string name;
    std::string type;
    std::string file_path;
    int line{};
    int end_line{};
    int complexity{};
    int lines_of_code{};
    int nesting_depth{};
    bool is_pure{};
    std::vector<std::string> side_effects;
    std::string content;
};

/// Represents the difference in symbols between two indexes.
struct SymbolDiff {
    std::vector<SymbolInfo> added;
    std::vector<SymbolInfo> removed;
    std::vector<SymbolInfo> modified;
};

/// Identifies a specific location in code.
struct CodeLocation {
    std::string file_path;
    int start_line{};
    int end_line{};
    std::string symbol_name;
    std::string snippet;
};

// ============================================================================
// Naming Analysis Types
// ============================================================================

/// Categorizes naming consistency issues.
enum class NamingIssueType : uint8_t {
    CaseMismatch,     ///< camelCase vs PascalCase vs snake_case mismatch
    SimilarExists,    ///< Similar names already exist in codebase
    Abbreviation,     ///< Abbreviation inconsistency (getUsr vs getUser)
};

/// Returns the string name for a NamingIssueType value.
constexpr std::string_view to_string(NamingIssueType t) {
    switch (t) {
        case NamingIssueType::CaseMismatch: return "case_mismatch";
        case NamingIssueType::SimilarExists: return "similar_exists";
        case NamingIssueType::Abbreviation: return "abbreviation";
    }
    return "unknown";
}

/// Represents a naming convention style.
enum class CaseStyle : uint8_t {
    CamelCase,
    PascalCase,
    SnakeCase,
    KebabCase,
    Unknown,
};

/// Returns the string name for a CaseStyle value.
constexpr std::string_view to_string(CaseStyle s) {
    switch (s) {
        case CaseStyle::CamelCase: return "camelCase";
        case CaseStyle::PascalCase: return "PascalCase";
        case CaseStyle::SnakeCase: return "snake_case";
        case CaseStyle::KebabCase: return "kebab-case";
        case CaseStyle::Unknown: return "unknown";
    }
    return "unknown";
}

/// Represents a programming language for naming conventions.
enum class Language : uint8_t {
    Go,
    JavaScript,
    TypeScript,
    Python,
    Rust,
    Java,
    CSharp,
    Cpp,
    C,
    PHP,
    Ruby,
    Swift,
    Kotlin,
    Scala,
    Zig,
    Unknown,
};

/// Categorizes symbols for naming convention purposes.
enum class SymbolKind : uint8_t {
    Function,
    Method,
    Class,
    Interface,
    Struct,
    Type,
    Constant,
    Variable,
    Field,
    Enum,
    EnumMember,
    Module,
    Namespace,
    Property,
    UnknownKind,
};

/// Defines expected case styles for a language.
struct NamingConvention {
    absl::flat_hash_map<SymbolKind, std::vector<CaseStyle>> expected_styles;
    std::string description;
};

/// Returns the language based on file extension.
Language get_language_from_path(std::string_view path);

/// Converts a symbol type string to SymbolKind.
SymbolKind symbol_type_to_kind(std::string_view symbol_type);

/// Returns the naming convention for a language, or nullptr if unknown.
const NamingConvention* get_naming_convention(Language lang);

/// Checks if a case style is valid for a symbol in a language.
bool is_valid_case_style(Language lang, SymbolKind kind, CaseStyle style);

/// Returns the expected case styles for a symbol kind in a language.
std::vector<CaseStyle> get_expected_styles(Language lang, SymbolKind kind);

/// Determines the case style of a name.
CaseStyle detect_case_style(std::string_view name);

// ============================================================================
// Metrics Analysis Types
// ============================================================================

/// Categorizes function metrics issues.
enum class MetricsIssueType : uint8_t {
    HighComplexity,
    LongFunction,
    DeepNesting,
    ComplexityGrew,
    PurityLost,
    ImpureFunction,
};

/// Returns the string name for a MetricsIssueType value.
constexpr std::string_view to_string(MetricsIssueType t) {
    switch (t) {
        case MetricsIssueType::HighComplexity: return "high_complexity";
        case MetricsIssueType::LongFunction: return "long_function";
        case MetricsIssueType::DeepNesting: return "deep_nesting";
        case MetricsIssueType::ComplexityGrew: return "complexity_grew";
        case MetricsIssueType::PurityLost: return "purity_lost";
        case MetricsIssueType::ImpureFunction: return "impure_function";
    }
    return "unknown";
}

/// Defines thresholds for metrics analysis.
struct MetricsThresholds {
    int high_complexity{10};
    int long_function{100};
    int deep_nesting{4};
    int complexity_growth_threshold{50};

    static MetricsThresholds defaults() {
        return {};
    }
};

/// Captures metrics for a function/method.
struct SymbolMetrics {
    int complexity{};
    int lines_of_code{};
    int nesting_depth{};
    bool is_pure{};
    std::vector<std::string> side_effects;
};

// ============================================================================
// Finding Types (from results.go)
// ============================================================================

/// Indicates the importance of a finding.
enum class FindingSeverity : uint8_t {
    Critical,
    Warning,
    Info,
};

/// Returns the string name for a FindingSeverity value.
constexpr std::string_view to_string(FindingSeverity s) {
    switch (s) {
        case FindingSeverity::Critical: return "critical";
        case FindingSeverity::Warning: return "warning";
        case FindingSeverity::Info: return "info";
    }
    return "unknown";
}

/// Represents detected duplicate code.
struct DuplicateFinding {
    FindingSeverity severity{FindingSeverity::Info};
    std::string description;
    CodeLocation new_code;
    CodeLocation existing_code;
    double similarity{};
    std::string type;
    std::string suggestion;
};

/// Represents a naming consistency issue.
struct NamingFinding {
    FindingSeverity severity{FindingSeverity::Info};
    std::string description;
    SymbolInfo new_symbol;
    std::vector<SymbolInfo> similar_names;
    NamingIssueType issue_type{NamingIssueType::CaseMismatch};
    std::string issue;
    std::string suggestion;
};

/// Represents a metrics-related issue in changed code.
struct MetricsFinding {
    FindingSeverity severity{FindingSeverity::Info};
    std::string description;
    SymbolInfo symbol;
    MetricsIssueType issue_type{MetricsIssueType::HighComplexity};
    std::string issue;
    std::string suggestion;
    SymbolMetrics* old_metrics{};
    SymbolMetrics* new_metrics{};
};

/// Provides context about the analysis.
struct ReportMetadata {
    std::string base_ref;
    std::string target_ref;
    AnalysisScope scope{AnalysisScope::Staged};
    std::chrono::system_clock::time_point analyzed_at;
    int64_t analysis_time_ms{};
    bool truncated{};
    int total_duplicates{};
    int total_naming_issues{};
    int total_metrics_issues{};
};

/// Provides high-level statistics about the analysis.
struct ReportSummary {
    int files_changed{};
    int symbols_added{};
    int symbols_modified{};
    int symbols_deleted{};
    int duplicates_found{};
    int naming_issues_found{};
    int metrics_issues_found{};
    double risk_score{};
    std::string top_recommendation;
};

/// The complete output of git change analysis.
struct AnalysisReport {
    ReportSummary summary;
    std::vector<DuplicateFinding> duplicates;
    std::vector<NamingFinding> naming_issues;
    std::vector<MetricsFinding> metrics_issues;
    ReportMetadata metadata;
};

// ============================================================================
// Risk / Severity Calculation Helpers
// ============================================================================

/// Computes an overall risk score based on findings.
double calculate_risk_score(const std::vector<DuplicateFinding>& duplicates,
                            const std::vector<NamingFinding>& naming_issues,
                            const std::vector<MetricsFinding>& metrics_issues);

/// Determines severity based on similarity and size.
FindingSeverity determine_duplicate_severity(double similarity, int line_count);

/// Determines severity based on issue type and context.
FindingSeverity determine_naming_severity(NamingIssueType issue_type, double similarity);

/// Determines severity based on metrics thresholds.
FindingSeverity determine_metrics_severity(MetricsIssueType issue_type,
                                           const SymbolMetrics& metrics,
                                           const MetricsThresholds& thresholds);

/// Creates the highest-priority recommendation.
std::string generate_top_recommendation(const std::vector<DuplicateFinding>& duplicates,
                                        const std::vector<NamingFinding>& naming_issues,
                                        const std::vector<MetricsFinding>& metrics_issues);

// ============================================================================
// Anti-Pattern Types (used by pattern_detector)
// ============================================================================

/// Categorizes conflict-prone code patterns.
enum class AntiPatternType : uint8_t {
    RegistrationFunction,
    EnumAggregation,
    GodObject,
    BarrelFile,
    SwitchFactory,
    ConfigAggregation,
};

/// Returns the string name for an AntiPatternType value.
constexpr std::string_view to_string(AntiPatternType t) {
    switch (t) {
        case AntiPatternType::RegistrationFunction: return "registration_function";
        case AntiPatternType::EnumAggregation: return "enum_aggregation";
        case AntiPatternType::GodObject: return "god_object";
        case AntiPatternType::BarrelFile: return "barrel_file";
        case AntiPatternType::SwitchFactory: return "switch_factory";
        case AntiPatternType::ConfigAggregation: return "config_aggregation";
    }
    return "unknown";
}

/// Indicates how problematic a pattern is.
enum class AntiPatternSeverity : uint8_t {
    High,
    Medium,
    Low,
};

/// Returns the string name for an AntiPatternSeverity value.
constexpr std::string_view to_string(AntiPatternSeverity s) {
    switch (s) {
        case AntiPatternSeverity::High: return "high";
        case AntiPatternSeverity::Medium: return "medium";
        case AntiPatternSeverity::Low: return "low";
    }
    return "unknown";
}

/// Represents a detected conflict-prone code pattern.
struct AntiPattern {
    AntiPatternType type{AntiPatternType::GodObject};
    std::string description;
    std::string location;
    AntiPatternSeverity severity{AntiPatternSeverity::Low};
    std::string suggestion;
    absl::flat_hash_map<std::string, int> metrics;
};

}  // namespace git
}  // namespace lci
