#include <lci/analysis/token_budget.h>

#include <algorithm>

namespace lci {

int TokenBudgetManager::calculate_target_budget(const int* max_results) {
    constexpr int kBaseBudget = 8000;

    if (max_results != nullptr && *max_results > 0) {
        double scale = static_cast<double>(*max_results) / 50.0;
        int adjusted = static_cast<int>(static_cast<double>(kBaseBudget) * scale);
        return std::clamp(adjusted, 4000, 12000);
    }

    return kBaseBudget;
}

int TokenBudgetManager::estimate_response_tokens(
    const CodebaseIntelligenceResponse& response) {
    int total = 0;

    if (response.repository_map != nullptr) {
        total += estimate_repository_map_tokens(*response.repository_map);
    }
    if (response.health_dashboard != nullptr) {
        total += estimate_health_dashboard_tokens(*response.health_dashboard);
    }
    if (response.entry_points != nullptr) {
        total += estimate_entry_points_tokens(*response.entry_points);
    }
    if (response.semantic_vocabulary != nullptr) {
        total += estimate_semantic_vocabulary_tokens(
            *response.semantic_vocabulary);
    }

    total += 200;  // metadata overhead
    return total;
}

int TokenBudgetManager::estimate_repository_map_tokens(
    const RepositoryMap& map) {
    int tokens = 50;
    tokens += static_cast<int>(map.critical_functions.size()) * 100;
    tokens += static_cast<int>(map.module_boundaries.size()) * 80;
    tokens += static_cast<int>(map.domain_terms.size()) * 50;
    tokens += static_cast<int>(map.entry_points.size()) * 60;
    return tokens;
}


int TokenBudgetManager::estimate_health_dashboard_tokens(
    const HealthDashboard& health) {
    int tokens = 100;
    tokens += 200;  // complexity metrics
    tokens += static_cast<int>(health.complexity.high_complexity_funcs.size()) * 80;
    tokens += static_cast<int>(health.technical_debt.components.size()) * 60;
    tokens += static_cast<int>(health.hotspots.size()) * 100;
    if (health.purity_summary != nullptr) {
        tokens += 60;
    }
    return tokens;
}

int TokenBudgetManager::estimate_entry_points_tokens(
    const EntryPointsList& points) {
    int tokens = 50;
    tokens += static_cast<int>(points.main_functions.size()) * 80;
    return tokens;
}

int TokenBudgetManager::estimate_semantic_vocabulary_tokens(
    const SemanticVocabulary& vocab) {
    int tokens = 50;
    tokens += static_cast<int>(vocab.domains_present.size()) * 60;
    tokens += static_cast<int>(vocab.unique_terms.size()) * 50;
    tokens += static_cast<int>(vocab.common_terms.size()) * 40;
    tokens += 30;  // analysis scope
    tokens += 10;  // vocabulary size
    return tokens;
}

void TokenBudgetManager::enforce_budget(
    CodebaseIntelligenceResponse& response, const int* max_results) {
    int target = calculate_target_budget(max_results);
    int current = estimate_response_tokens(response);
    if (current <= target) return;
    truncate_to_budget(response, target);
}

void TokenBudgetManager::truncate_to_budget(
    CodebaseIntelligenceResponse& response, int target_tokens) {

    // Strategy 1: Reduce critical functions
    if (response.repository_map != nullptr &&
        !response.repository_map->critical_functions.empty()) {
        int current = estimate_response_tokens(response);
        if (current > target_tokens) {
            double factor =
                static_cast<double>(target_tokens) / static_cast<double>(current);
            int reduced = static_cast<int>(
                static_cast<double>(
                    response.repository_map->critical_functions.size()) *
                factor * 0.8);
            if (reduced < 5) reduced = 5;
            auto& funcs = response.repository_map->critical_functions;
            if (reduced < static_cast<int>(funcs.size())) {
                funcs.resize(static_cast<size_t>(reduced));
            }
        }
    }

    // Strategy 2: Reduce hotspots
    if (response.health_dashboard != nullptr &&
        response.health_dashboard->hotspots.size() > 10) {
        int current = estimate_response_tokens(response);
        if (current > target_tokens) {
            response.health_dashboard->hotspots.resize(10);
        }
    }

    // Strategy 3: Reduce module boundaries
    if (response.repository_map != nullptr &&
        response.repository_map->module_boundaries.size() > 15) {
        int current = estimate_response_tokens(response);
        if (current > target_tokens) {
            response.repository_map->module_boundaries.resize(15);
        }
    }

    // Emergency truncation
    int current = estimate_response_tokens(response);
    if (current > target_tokens) {
        if (response.repository_map != nullptr) {
            auto& funcs = response.repository_map->critical_functions;
            if (funcs.size() > 5) funcs.resize(5);
            response.repository_map->module_boundaries.clear();
            response.repository_map->domain_terms.clear();
            response.repository_map->entry_points.clear();
        }

        if (response.health_dashboard != nullptr &&
            response.health_dashboard->hotspots.size() > 3) {
            response.health_dashboard->hotspots.resize(3);
        }
    }
}

}  // namespace lci
