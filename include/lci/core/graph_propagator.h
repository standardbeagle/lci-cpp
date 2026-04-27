#pragma once

#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/types.h>

namespace lci {

class ReferenceTracker;

// ---------------------------------------------------------------------------
// Propagation mode
// ---------------------------------------------------------------------------

enum class PropagationMode : uint8_t {
    Reachability = 0,
    Accumulation,
    Decay,
    Max,
};

// ---------------------------------------------------------------------------
// Propagation configuration types
// ---------------------------------------------------------------------------

struct LabelPropagationRule {
    std::string label;
    std::string direction;
    PropagationMode mode{PropagationMode::Decay};
    double decay{0.8};
    int max_hops{};
    double min_strength{};
    double boost{};
    int priority{};
    bool include_type_hierarchy{};
};

struct DependencyPropagationRule {
    std::string dependency_type;
    std::string direction;
    std::string aggregation;
    std::string weight_function;
    int max_depth{};
    double threshold{};
};

struct PropagationConfig {
    int max_iterations{10};
    double convergence_threshold{0.001};
    double default_decay{0.8};
    std::vector<LabelPropagationRule> label_rules;
    std::vector<DependencyPropagationRule> dependency_rules;
};

// ---------------------------------------------------------------------------
// Propagation state
// ---------------------------------------------------------------------------

struct PropagationKey {
    SymbolID symbol_id{};
    std::string attribute;
    std::string type;

    bool operator==(const PropagationKey& o) const {
        return symbol_id == o.symbol_id && attribute == o.attribute &&
               type == o.type;
    }
};

struct PropagationKeyHash {
    size_t operator()(const PropagationKey& k) const {
        size_t h = std::hash<uint64_t>{}(k.symbol_id);
        h ^= std::hash<std::string>{}(k.attribute) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(k.type) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct PropagationValue {
    double strength{};
    SymbolID source{};
    int hops{};
    std::vector<SymbolID> path;
    int last_updated{};
};

struct PropagatedLabel {
    std::string label;
    double strength{};
    SymbolID source{};
    int hops{};
    double confidence{};
};

// ---------------------------------------------------------------------------
// GraphPropagator - multi-mode label and dependency propagation
// ---------------------------------------------------------------------------

/// Propagates labels and dependencies through a symbol call graph.
///
/// Supports four modes matching the Go implementation:
///  - Reachability: binary, all reachable get strength 1.0
///  - Accumulation: strength sums upward
///  - Decay: PageRank-style strength decay per hop
///  - Max: take maximum strength along any path
class GraphPropagator {
  public:
    explicit GraphPropagator(const ReferenceTracker* ref_tracker);

    void set_config(PropagationConfig config);

    /// Seeds a label at a specific symbol with the given strength.
    void seed_label(SymbolID symbol_id, std::string_view label,
                    double strength = 1.0);

    /// Runs the iterative propagation until convergence or max iterations.
    void propagate();

    /// Returns propagated labels for a symbol.
    std::vector<PropagatedLabel> get_labels(SymbolID symbol_id) const;

    /// Returns the number of iterations performed.
    int iteration_count() const { return iteration_count_; }

    /// Returns whether the propagation converged.
    bool converged() const { return converged_; }

    /// Returns all propagation state entries (for testing).
    int state_size() const {
        return static_cast<int>(state_.size());
    }

  private:
    bool run_iteration();
    void propagate_label(const LabelPropagationRule& rule,
        absl::flat_hash_map<PropagationKey, PropagationValue, PropagationKeyHash>& new_state);
    double calculate_propagated_strength(const LabelPropagationRule& rule,
                                         const PropagationValue& source,
                                         double existing_strength) const;
    std::vector<SymbolID> get_connected_symbols(SymbolID symbol_id,
                                                 std::string_view direction) const;
    void check_convergence(
        const absl::flat_hash_map<PropagationKey, PropagationValue, PropagationKeyHash>& old_state);

    const ReferenceTracker* ref_tracker_{};
    PropagationConfig config_;
    absl::flat_hash_map<PropagationKey, PropagationValue, PropagationKeyHash> state_;
    absl::flat_hash_map<SymbolID, std::vector<PropagatedLabel>> label_cache_;
    int iteration_count_{};
    bool converged_{};
};

/// Returns a default propagation configuration matching the Go implementation.
PropagationConfig default_propagation_config();

}  // namespace lci
