#include <lci/core/graph_propagator.h>
#include <lci/core/reference_tracker.h>

#include <algorithm>
#include <cmath>

namespace lci {

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

PropagationConfig default_propagation_config() {
    PropagationConfig cfg;
    cfg.max_iterations = 10;
    cfg.convergence_threshold = 0.001;
    cfg.default_decay = 0.8;
    cfg.label_rules = {
        LabelPropagationRule{
            "critical", "upstream", PropagationMode::Reachability,
            0.8, 0, 0.0, 0.0, 3, true},
        LabelPropagationRule{
            "security", "upstream", PropagationMode::Reachability,
            0.8, 0, 0.0, 0.0, 3, true},
    };
    return cfg;
}

// ---------------------------------------------------------------------------
// GraphPropagator
// ---------------------------------------------------------------------------

GraphPropagator::GraphPropagator(const ReferenceTracker* ref_tracker)
    : ref_tracker_(ref_tracker), config_(default_propagation_config()) {}

void GraphPropagator::set_config(PropagationConfig config) {
    config_ = std::move(config);
}

void GraphPropagator::seed_label(SymbolID symbol_id, std::string_view label,
                                 double strength) {
    PropagationKey key{symbol_id, std::string(label), "label"};
    state_[key] = PropagationValue{
        strength, symbol_id, 0, {symbol_id}, 0};
}

void GraphPropagator::propagate() {
    iteration_count_ = 0;
    converged_ = false;
    label_cache_.clear();

    for (int i = 0; i < config_.max_iterations && !converged_; ++i) {
        auto old_state = state_;
        bool changed = run_iteration();
        iteration_count_++;

        if (!changed) {
            converged_ = true;
        } else {
            check_convergence(old_state);
        }
    }

    // Build label cache for fast lookups
    for (const auto& [key, value] : state_) {
        if (key.type == "label") {
            label_cache_[key.symbol_id].push_back(PropagatedLabel{
                key.attribute, value.strength, value.source,
                value.hops, value.strength});
        }
    }
}

std::vector<PropagatedLabel> GraphPropagator::get_labels(
    SymbolID symbol_id) const {
    auto it = label_cache_.find(symbol_id);
    if (it != label_cache_.end()) return it->second;
    return {};
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

bool GraphPropagator::run_iteration() {
    absl::flat_hash_map<PropagationKey, PropagationValue, PropagationKeyHash>
        new_state(state_);

    for (const auto& rule : config_.label_rules) {
        propagate_label(rule, new_state);
    }

    bool changed = (new_state.size() != state_.size());
    if (!changed) {
        for (const auto& [key, value] : new_state) {
            auto it = state_.find(key);
            if (it == state_.end() ||
                std::abs(it->second.strength - value.strength) > 1e-10) {
                changed = true;
                break;
            }
        }
    }

    state_ = std::move(new_state);
    return changed;
}

void GraphPropagator::propagate_label(
    const LabelPropagationRule& rule,
    absl::flat_hash_map<PropagationKey, PropagationValue, PropagationKeyHash>&
        new_state) {
    // Collect entries matching this label (iterate over copy to avoid aliasing)
    std::vector<std::pair<PropagationKey, PropagationValue>> sources;
    for (const auto& [key, value] : state_) {
        if (key.type == "label" && key.attribute == rule.label) {
            if (rule.max_hops > 0 && value.hops >= rule.max_hops) continue;
            if (rule.mode == PropagationMode::Decay &&
                value.strength < rule.min_strength) {
                continue;
            }
            sources.emplace_back(key, value);
        }
    }

    for (const auto& [key, value] : sources) {
        auto targets = get_connected_symbols(key.symbol_id, rule.direction);

        for (SymbolID target_id : targets) {
            PropagationKey target_key{target_id, rule.label, "label"};

            double existing = 0.0;
            auto it = new_state.find(target_key);
            if (it != new_state.end()) existing = it->second.strength;

            double new_strength =
                calculate_propagated_strength(rule, value, existing);

            bool should_update = false;
            if (it == new_state.end()) {
                should_update = true;
            } else {
                switch (rule.mode) {
                    case PropagationMode::Reachability:
                        should_update = false;
                        break;
                    case PropagationMode::Accumulation:
                        should_update = true;
                        break;
                    case PropagationMode::Decay:
                        should_update = new_strength > existing;
                        break;
                    case PropagationMode::Max:
                        should_update = new_strength > existing;
                        break;
                }
            }

            if (should_update) {
                auto path = value.path;
                path.push_back(target_id);

                double final_strength = new_strength;
                if (rule.mode == PropagationMode::Accumulation && it != new_state.end()) {
                    final_strength = existing + new_strength;
                }

                new_state[target_key] = PropagationValue{
                    final_strength, value.source,
                    value.hops + 1, std::move(path), iteration_count_};
            }
        }
    }
}

double GraphPropagator::calculate_propagated_strength(
    const LabelPropagationRule& rule, const PropagationValue& source,
    double existing_strength) const {
    switch (rule.mode) {
        case PropagationMode::Reachability:
            return 1.0;
        case PropagationMode::Accumulation:
            return source.strength;
        case PropagationMode::Decay: {
            double decay = rule.decay > 0 ? rule.decay : config_.default_decay;
            double result = source.strength * decay;
            if (rule.boost > 0) result *= rule.boost;
            return result;
        }
        case PropagationMode::Max:
            return source.strength;
    }
    return 0.0;
}

std::vector<SymbolID> GraphPropagator::get_connected_symbols(
    SymbolID symbol_id, std::string_view direction) const {
    if (!ref_tracker_) return {};

    std::vector<SymbolID> result;

    if (direction == "upstream" || direction == "bidirectional") {
        auto callers = ref_tracker_->get_caller_symbols(symbol_id);
        result.insert(result.end(), callers.begin(), callers.end());
    }
    if (direction == "downstream" || direction == "bidirectional") {
        auto callees = ref_tracker_->get_callee_symbols(symbol_id);
        result.insert(result.end(), callees.begin(), callees.end());
    }

    return result;
}

void GraphPropagator::check_convergence(
    const absl::flat_hash_map<PropagationKey, PropagationValue,
                              PropagationKeyHash>& old_state) {
    double max_change = 0.0;

    for (const auto& [key, value] : state_) {
        auto it = old_state.find(key);
        if (it == old_state.end()) {
            max_change = std::max(max_change, std::abs(value.strength));
        } else {
            max_change = std::max(
                max_change, std::abs(value.strength - it->second.strength));
        }
    }

    converged_ = max_change < config_.convergence_threshold;
}

}  // namespace lci
