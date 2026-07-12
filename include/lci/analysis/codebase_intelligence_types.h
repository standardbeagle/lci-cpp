#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace lci {

struct EnhancedSymbol;

/// Pairs a file path with its enhanced symbols for analysis input.
/// Bridges the gap between FileInfo (no embedded symbols) and analysis APIs.
struct FileSymbolData {
    std::string path;
    std::vector<const EnhancedSymbol*> symbols;
    /// Optional ownership token for symbol pointers sourced from an RCU view.
    std::shared_ptr<const void> owner;
};

// ============================================================================
// Thresholds (matching Go constants)
// ============================================================================

namespace ci_thresholds {
inline constexpr int kComplexityLow = 10;
inline constexpr int kComplexityModerate = 15;
inline constexpr int kComplexityHigh = 20;
inline constexpr int kHotspotComplexity = 10;
inline constexpr int kHotspotLinecount = 50;
inline constexpr int kHighReferenceCount = 10;
inline constexpr int kHighUsage = 5;
inline constexpr double kRiskScoreMax = 10.0;
inline constexpr double kMaintainabilityMin = 0.0;
inline constexpr double kMaintainabilityMax = 100.0;
inline constexpr int kLongFunction = 50;
inline constexpr int kLongFunctionHighSev = 100;
inline constexpr int kHighComplexity = 10;
inline constexpr int kHighComplexityHighSev = 20;
inline constexpr int kGodClass = 15;
inline constexpr int kGodClassHighSev = 25;
inline constexpr int kShotgunSurgery = 10;
inline constexpr int kShotgunSurgeryHighSev = 20;
inline constexpr int kRiskScoreCutoff = 5;
inline constexpr int kMaxDetailedSmells = 5;
inline constexpr int kMaxProblematicSymbols = 5;
}  // namespace ci_thresholds

// ============================================================================
// Tier 1: High-Level Overview Types
// ============================================================================

/// A code hotspot (high complexity area).
struct Hotspot {
    std::string location;
    double complexity{};
    double risk_score{};
};

/// High complexity function info for drill-down.
struct FunctionInfo {
    std::string object_id;
    std::string entity_id;
    std::string name;
    std::string location;
    std::string file_id;
    double complexity{};
    int line_count{};
};

/// Complexity distribution metrics.
struct ComplexityMetrics {
    double average_cc{};
    double median_cc{};
    absl::flat_hash_map<std::string, double> percentiles;
    std::vector<FunctionInfo> high_complexity_funcs;
    absl::flat_hash_map<std::string, int> distribution;
};

/// Technical debt analysis.
struct TechnicalDebtMetrics {
    double ratio{};
    std::string estimate;
    std::vector<std::string> components;
};

/// Analysis metadata.
struct AnalysisMetadata {
    int analysis_time_ms{};
    int files_analyzed{};
    std::chrono::system_clock::time_point analyzed_at;
    std::string index_version;
};

/// Detected code smell with drill-down capability.
struct CodeSmellEntry {
    std::string type;
    std::string symbol;
    std::string object_id;
    std::string location;
    std::string severity;
    std::string description;
};

/// Symbol with quality issues and drill-down capability.
struct ProblematicSymbol {
    std::string object_id;
    std::string name;
    std::string location;
    int risk_score{};
    std::vector<std::string> tags;
};

/// Quality metrics.
struct QualityMetrics {
    double maintainability_index{};
    double technical_debt_ratio{};
};

/// Function purity summary.
struct PuritySummary {
    int total_functions{};
    int pure_functions{};
    int impure_functions{};
    double purity_ratio{};
    int with_param_writes{};
    int with_global_writes{};
    int with_io_effects{};
    int with_throws{};
    int with_external_calls{};
    std::string detailed_query;
};

/// Performance anti-pattern.
struct PerformanceAntiPattern {
    std::string type;
    std::string symbol;
    std::string object_id;
    std::string location;
    std::string severity;
    std::string description;
    std::string language;
    std::string suggestion;
};

/// Performance analysis summary.
struct PerformanceSummary {
    int total_patterns{};
    absl::flat_hash_map<std::string, int> by_severity;
    absl::flat_hash_map<std::string, int> by_type;
    absl::flat_hash_map<std::string, int> by_language;
    std::vector<std::string> most_affected_files;
};

/// Performance analysis results.
struct PerformanceAnalysis {
    std::vector<PerformanceAntiPattern> patterns;
    PerformanceSummary summary;
    AnalysisMetadata analysis_metadata;
};

/// Health dashboard combining all health metrics.
struct HealthDashboard {
    double overall_score{};
    ComplexityMetrics complexity;
    TechnicalDebtMetrics technical_debt;
    std::vector<Hotspot> hotspots;
    AnalysisMetadata analysis_metadata;
    std::vector<CodeSmellEntry> detailed_smells;
    std::vector<ProblematicSymbol> problematic_symbols;
    absl::flat_hash_map<std::string, int> smell_counts;
    PerformanceAnalysis* performance_patterns{};
    PuritySummary* purity_summary{};
};

// ============================================================================
// Domain Vocabulary Types
// ============================================================================

/// Domain-specific term cluster.
struct DomainTerm {
    std::string domain;
    std::vector<std::string> terms;
    double confidence{};
    int count{};
};

/// Domain pattern for classification.
struct DomainPattern {
    std::vector<std::string> keywords;
    double exact_weight{};
    double prefix_weight{};
};

/// Semantic domain found in codebase.
struct SemanticDomain {
    std::string name;
    int count{};
    double confidence{};
    std::vector<std::string> example_symbols;
    std::vector<std::string> matched_terms;
};

/// Vocabulary term.
struct SemanticTerm {
    std::string term;
    int count{};
    std::vector<std::string> example_symbols;
    std::vector<std::string> domains;
};

/// Vocabulary scope information.
struct VocabularyScope {
    int total_files{};
    int production_files{};
    int test_files_excluded{};
    std::vector<std::string> source_directories;
    int total_symbols{};
    int total_functions{};
    int total_variables{};
    int total_types{};
};

/// Semantic vocabulary analysis results.
struct SemanticVocabulary {
    std::vector<SemanticDomain> domains_present;
    std::vector<std::string> domains_absent;
    std::vector<SemanticTerm> unique_terms;
    std::vector<SemanticTerm> common_terms;
    VocabularyScope analysis_scope;
    int vocabulary_size{};
};

// ============================================================================
// Tier 1: Repository Map Types
// ============================================================================

/// Symbol reference for quick browsing.
struct SymbolRef {
    std::string object_id;
    std::string entity_id;
    std::string name;
    std::string symbol_type;
    std::string location;
    std::string file_id;
    int complexity{};
};

/// Critical function signature.
struct FunctionSignature {
    std::string object_id;
    std::string entity_id;
    std::string name;
    std::string module;
    std::string signature;
    double importance_score{};
    int referenced_by{};
    std::string symbol_type;
    bool is_exported{};
    std::string file_id;
    std::string location;
};

/// Module boundary with metrics.
struct ModuleBoundary {
    std::string entity_id;
    std::string name;
    std::string type;
    std::string path;
    double cohesion_score{};
    double coupling_score{};
    double stability{};
    int file_count{};
    int function_count{};
    std::vector<std::string> file_ids;
    std::vector<std::string> function_ids;
    std::vector<std::string> class_ids;
    std::vector<SymbolRef> example_functions;
    std::vector<SymbolRef> example_classes;
};

/// Entry point.
struct EntryPointDef {
    std::string object_id;
    std::string entity_id;
    std::string name;
    std::string type;
    std::string location;
    std::string signature;
    bool is_exported{};
    std::string file_id;
    double importance{};
};

/// Repository map.
struct RepositoryMap {
    std::vector<FunctionSignature> critical_functions;
    std::vector<ModuleBoundary> module_boundaries;
    std::vector<DomainTerm> domain_terms;
    std::vector<EntryPointDef> entry_points;
    std::chrono::system_clock::time_point analyzed_at;
    int total_files{};
    int total_functions{};
    int total_symbols{};
    std::string note;
};

/// Entry points list.
struct EntryPointsList {
    std::vector<EntryPointDef> main_functions;
};

// ============================================================================
// Coupling & Cohesion Types (Tier 3 statistics)
// ============================================================================

/// Coupling metrics for package-level analysis.
struct CouplingMetrics {
    absl::flat_hash_map<std::string, int> afferent_coupling;
    absl::flat_hash_map<std::string, int> efferent_coupling;
    absl::flat_hash_map<std::string, double> instability;
    absl::flat_hash_map<std::string, double> module_coupling;
    double average_coupling{};
    double max_coupling{};
};

/// Cohesion metrics for package-level analysis.
struct CohesionMetrics {
    absl::flat_hash_map<std::string, double> relational_cohesion;
    double average_cohesion{};
    double min_cohesion{1.0};
    std::vector<std::string> low_cohesion_modules;
};

// ============================================================================
// Violation Type
// ============================================================================

/// Detected architectural violation.
struct Violation {
    std::string type;
    std::string severity;
    std::string description;
    std::string location;
};

// ============================================================================
// Module Analysis Types (Tier 2)
// ============================================================================

/// Module analysis metrics.
struct ModuleAnalysisMetrics {
    int total_modules{};
    double average_cohesion{};
    double average_coupling{};
    double architectural_score{};
};

/// Module analysis results.
struct ModuleAnalysis {
    std::vector<ModuleBoundary> modules;
    std::string detection_strategy;
    absl::flat_hash_map<std::string, int> module_types;
    absl::flat_hash_map<std::string, std::vector<std::string>> layer_distribution;
    std::vector<Violation> violations;
    ModuleAnalysisMetrics metrics;
};

// ============================================================================
// Layer Analysis Types (Tier 2)
// ============================================================================

/// Metrics for a single architectural layer.
struct LayerMetricsData {
    int module_count{};
    double cohesion_score{};
    double coupling_score{};
    double maintainability{};
    double complexity{};
};

/// Detected architectural layer.
struct ArchitecturalLayer {
    std::string name;
    std::vector<std::string> modules;
    int depth{};
    std::vector<std::string> component_types;
    LayerMetricsData metrics;
};

/// Detected architectural pattern.
struct LayerPattern {
    std::string name;
    std::string description;
    double confidence{};
    std::vector<std::string> violations;
};

/// Layer analysis results.
struct LayerAnalysis {
    std::vector<ArchitecturalLayer> layers;
    int violation_count{};
    std::vector<LayerMetricsData> layer_metrics;
    std::vector<LayerPattern> patterns;
    std::vector<std::vector<double>> dependency_matrix;
};

// ============================================================================
// Feature Analysis Types (Tier 2)
// ============================================================================

/// A code component within a feature.
struct ComponentInfo {
    std::string name;
    std::string type;
    std::string location;
    double complexity{};
    std::vector<std::string> dependencies;
};

/// Cross-feature dependency.
struct FeatureDependency {
    std::string feature_a;
    std::string feature_b;
    std::string type;
    double strength{};
};

/// Feature analysis metrics.
///
/// Features are Louvain communities of the symbol reference graph, so these are
/// real graph quantities: `avg_cohesion` is the mean per-feature internal-edge
/// fraction — internal / (internal + boundary) edges, in [0, 1] — and
/// `avg_complexity` is the mean per-feature average cyclomatic complexity of its
/// member symbols. (Earlier revisions computed name-prefix cohesion and a
/// name-length proxy here; both are gone.)
struct FeatureAnalysisMetrics {
    int total_features{};
    double average_components{};
    double avg_cohesion{};
    double avg_complexity{};
};

/// Detected feature.
struct Feature {
    std::string name;
    std::string primary_module;
    std::vector<ComponentInfo> components;
    double confidence{};
};

/// Feature analysis results.
struct FeatureAnalysis {
    std::vector<Feature> features;
    std::vector<FeatureDependency> cross_feature_deps;
    std::vector<ComponentInfo> orphan_components;
    FeatureAnalysisMetrics metrics;
};

// ============================================================================
// Statistics Report Type (Tier 3)
// ============================================================================

/// Aggregate statistics for the `statistics` mode: complexity distribution,
/// package coupling/cohesion, quality, and function purity.
struct StatisticsReport {
    ComplexityMetrics complexity;
    CouplingMetrics coupling;
    CohesionMetrics cohesion;
    QualityMetrics quality;
    double purity_ratio{};
};

// ============================================================================
// Structure Analysis Type (directory tree exploration)
// ============================================================================

/// Directory/file-type breakdown for the `structure` mode. `types` is sorted
/// by extension ascending; `top_dirs` is sorted by file count descending.
struct StructureAnalysis {
    int dir_count{};
    int file_count{};
    int symbol_count{};
    int max_depth{};
    std::vector<std::pair<std::string, int>> types;
    int code{};
    int tests{};
    int config{};
    int docs{};
    std::vector<std::pair<std::string, int>> top_dirs;
};

// ============================================================================
// Unified Response Type
// ============================================================================

/// Unified codebase intelligence response.
struct CodebaseIntelligenceResponse {
    RepositoryMap* repository_map{};
    HealthDashboard* health_dashboard{};
    EntryPointsList* entry_points{};
    SemanticVocabulary* semantic_vocabulary{};
    // Tier 2/3 analysis payloads. Value-owned (optional) so the response frees
    // them itself — unlike the raw-pointer sub-objects above. Populated by the
    // engine's detailed/statistics/structure builders; the MCP handler renders
    // LCF from these rather than recomputing the analysis inline.
    std::optional<ModuleAnalysis> module_analysis;
    std::optional<LayerAnalysis> layer_analysis;
    std::optional<FeatureAnalysis> feature_analysis;
    std::vector<DomainTerm> domain_terms;
    std::optional<StatisticsReport> statistics_report;
    std::optional<StructureAnalysis> structure_analysis;
    std::string analysis_mode;
    int tier{};
    AnalysisMetadata analysis_metadata;
    absl::flat_hash_map<std::string, std::string> navigation_hints;
};

}  // namespace lci
