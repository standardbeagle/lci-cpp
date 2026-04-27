#include <lci/analysis/metrics_calculator.h>

#include <algorithm>
#include <cmath>

namespace lci {

// ---------------------------------------------------------------------------
// MetricsCalculator
// ---------------------------------------------------------------------------

SymbolMetrics MetricsCalculator::calculate_symbol_metrics(
    SymbolID id, std::string_view name, SymbolType type, FileID file_id,
    int start_line, int end_line, int incoming_refs, int outgoing_refs) {

    // Check cache
    auto it = cache_.find(id);
    if (it != cache_.end() && !it->second.is_stale) {
        return it->second;
    }

    SymbolMetrics m{};
    m.symbol_id = id;
    m.name = std::string(name);
    m.type = type;
    m.file_id = file_id;

    m.quality.lines_of_code = (end_line > start_line) ? end_line - start_line + 1 : 1;

    // Dependency metrics
    m.dependencies.incoming_dependencies = incoming_refs;
    m.dependencies.outgoing_dependencies = outgoing_refs;

    int total = incoming_refs + outgoing_refs;
    if (total > 0) {
        m.dependencies.coupling_strength =
            std::min(1.0, static_cast<double>(total) / 100.0);
        m.dependencies.stability_index =
            static_cast<double>(incoming_refs) / static_cast<double>(total);
        m.dependencies.instability_index =
            static_cast<double>(outgoing_refs) / static_cast<double>(total);
    }

    m.quality.maintainability_index =
        compute_maintainability_index(m.quality.halstead_volume,
                                     m.quality.cyclomatic_complexity,
                                     m.quality.lines_of_code);

    m.risk_score = compute_risk_score(m);
    m.tags = generate_tags(m);

    cache_[id] = m;
    return m;
}

double MetricsCalculator::compute_maintainability_index(
    double halstead_volume, int cyclomatic_complexity, int lines_of_code) {
    if (halstead_volume <= 0 || lines_of_code <= 0) return 0.0;

    double mi = 171.0 - 5.2 * std::log(halstead_volume) -
                0.23 * static_cast<double>(cyclomatic_complexity) -
                16.2 * std::log(static_cast<double>(lines_of_code));

    return std::clamp(mi, 0.0, 100.0);
}

int MetricsCalculator::compute_risk_score(const SymbolMetrics& m) {
    int score = 0;

    // Complexity contribution (0-4 points)
    if (m.quality.cyclomatic_complexity > 20) score += 4;
    else if (m.quality.cyclomatic_complexity > 15) score += 3;
    else if (m.quality.cyclomatic_complexity > 10) score += 2;
    else if (m.quality.cyclomatic_complexity > 5) score += 1;

    // Dependency contribution (0-3 points)
    int total_deps = m.dependencies.incoming_dependencies +
                     m.dependencies.outgoing_dependencies;
    if (total_deps > 20) score += 3;
    else if (total_deps > 10) score += 2;
    else if (total_deps > 5) score += 1;

    // Maintainability contribution (0-3 points)
    if (m.quality.maintainability_index < 10) score += 3;
    else if (m.quality.maintainability_index < 25) score += 2;
    else if (m.quality.maintainability_index < 50) score += 1;

    return std::min(score, 10);
}

std::vector<std::string> MetricsCalculator::generate_tags(
    const SymbolMetrics& m) {
    std::vector<std::string> tags;

    if (m.quality.cyclomatic_complexity > 15)
        tags.emplace_back("HIGH_COMPLEXITY");
    if (m.quality.cognitive_complexity > 25)
        tags.emplace_back("HIGH_COGNITIVE_LOAD");
    if (m.quality.nesting_depth > 5)
        tags.emplace_back("DEEP_NESTING");

    if (m.dependencies.incoming_dependencies > 10)
        tags.emplace_back("HIGHLY_COUPLED");
    if (m.dependencies.outgoing_dependencies > 15)
        tags.emplace_back("MANY_DEPENDENCIES");
    if (m.dependencies.has_circular_deps)
        tags.emplace_back("CIRCULAR_DEPS");

    if (m.type == SymbolType::Interface)
        tags.emplace_back("INTERFACE");

    if (m.risk_score >= 8)
        tags.emplace_back("HIGH_RISK");
    else if (m.risk_score >= 5)
        tags.emplace_back("MEDIUM_RISK");

    if (m.quality.maintainability_index < 10)
        tags.emplace_back("HARD_TO_MAINTAIN");
    if (m.quality.maintainability_index > 85)
        tags.emplace_back("WELL_MAINTAINED");

    return tags;
}

void MetricsCalculator::invalidate_cache() {
    for (auto& [_, m] : cache_) {
        m.is_stale = true;
    }
}

}  // namespace lci
