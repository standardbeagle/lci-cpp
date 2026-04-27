#pragma once

#include <cmath>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/types.h>

namespace lci {

/// Code quality measurements for a single symbol.
struct CodeQualityMetrics {
    int cyclomatic_complexity{};
    int cognitive_complexity{};
    int nesting_depth{};
    int lines_of_code{};
    int lines_of_comments{};
    double halstead_volume{};
    double halstead_difficulty{};
    double maintainability_index{};
};

/// Dependency measurements for a single symbol.
struct DependencyMetrics {
    int incoming_dependencies{};
    int outgoing_dependencies{};
    int transitive_dependencies{};
    double coupling_strength{};
    double cohesion_strength{1.0};
    int depth_in_call_tree{};
    bool has_circular_deps{};
    double stability_index{};
    double instability_index{};
};

/// Combined metrics for a symbol.
struct SymbolMetrics {
    SymbolID symbol_id{};
    std::string name;
    SymbolType type{};
    FileID file_id{};
    std::string file_path;

    CodeQualityMetrics quality;
    DependencyMetrics dependencies;

    std::vector<std::string> tags;
    int risk_score{};

    int64_t calculated_at{};
    bool is_stale{};
};

/// Computes comprehensive code quality and dependency metrics.
///
/// Caches results by SymbolID and invalidates on request.
class MetricsCalculator {
  public:
    MetricsCalculator() = default;

    /// Calculates or returns cached metrics for a symbol.
    SymbolMetrics calculate_symbol_metrics(SymbolID id, std::string_view name,
                                           SymbolType type, FileID file_id,
                                           int start_line, int end_line,
                                           int incoming_refs, int outgoing_refs);

    /// Computes maintainability index from quality metrics.
    static double compute_maintainability_index(
        double halstead_volume, int cyclomatic_complexity, int lines_of_code);

    /// Computes risk score (0-10) from metrics.
    static int compute_risk_score(const SymbolMetrics& m);

    /// Generates descriptive tags from metrics.
    static std::vector<std::string> generate_tags(const SymbolMetrics& m);

    /// Marks all cached entries as stale.
    void invalidate_cache();

    /// Returns number of cached entries.
    int cache_size() const { return static_cast<int>(cache_.size()); }

  private:
    absl::flat_hash_map<SymbolID, SymbolMetrics> cache_;
};

}  // namespace lci
