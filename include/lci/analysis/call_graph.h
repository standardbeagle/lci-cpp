#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/types.h>

namespace lci {
namespace analysis {

/// Directed call graph over a fixed node set, with strongly-connected-component
/// analysis. Edges run caller -> callee (a node points at what it depends on).
///
/// All algorithms are deterministic (neighbor lists sorted, components emitted
/// in a stable order) and avoid recursion (iterative Tarjan) so deep graphs
/// don't overflow the stack. Edge weight is a uniform 1.0 — for a call graph an
/// edge either carries dependence or it does not, so there is no per-hop decay.
class CallGraph {
  public:
    /// Builds the graph. `nodes` is the symbol set; `callees_of(id)` yields the
    /// callee SymbolIDs of `id`. Edges to symbols outside `nodes` are dropped.
    /// Runs Tarjan SCC as part of the build.
    void build(const std::vector<SymbolID>& nodes,
               const std::function<std::vector<SymbolID>(SymbolID)>& callees_of);

    int node_count() const { return static_cast<int>(ids_.size()); }
    SymbolID id_at(int idx) const { return ids_[idx]; }
    int component_count() const { return n_comps_; }

    /// Dense SCC id per node index (0-based). Edge u->v in the condensation
    /// implies component_of[u] > component_of[v] (reverse-topological).
    const std::vector<int>& component_of() const { return comp_; }

    /// Member node indices of every cyclic component (size > 1, or a self-loop),
    /// each member list sorted ascending; the outer list sorted by first member.
    /// A non-empty result means the codebase has circular call dependencies.
    std::vector<std::vector<int>> cycles() const;

    /// Transitive incoming reach per node: the number of *distinct other* nodes
    /// that can reach it through directed edges (its transitive callers). Exact,
    /// computed via SCC condensation + bitset ancestor closure — one pass for
    /// all nodes, O(C^2/64) space in the component count C. Weight 1.0 per edge.
    std::vector<int> incoming_reach() const;

  private:
    std::vector<SymbolID> ids_;                   // idx -> SymbolID
    absl::flat_hash_map<SymbolID, int> index_;    // SymbolID -> idx
    std::vector<std::vector<int>> adj_;           // idx -> sorted callee idxs
    std::vector<int> comp_;                       // idx -> SCC id
    int n_comps_{};
    bool has_self_loop_dummy_{};                  // reserved

    void tarjan_scc();
};

}  // namespace analysis
}  // namespace lci
