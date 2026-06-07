#include <lci/analysis/call_graph.h>

#include <algorithm>
#include <cstdint>

namespace lci {
namespace analysis {

void CallGraph::build(
    const std::vector<SymbolID>& nodes,
    const std::function<std::vector<SymbolID>(SymbolID)>& callees_of) {
    ids_.clear();
    index_.clear();
    adj_.clear();
    comp_.clear();
    n_comps_ = 0;

    // Deterministic dense indexing in the caller-provided node order.
    ids_.reserve(nodes.size());
    index_.reserve(nodes.size());
    for (SymbolID id : nodes) {
        if (index_.emplace(id, static_cast<int>(ids_.size())).second) {
            ids_.push_back(id);
        }
    }

    const int n = static_cast<int>(ids_.size());
    adj_.assign(n, {});
    for (int u = 0; u < n; ++u) {
        auto callees = callees_of(ids_[u]);
        auto& out = adj_[u];
        out.reserve(callees.size());
        for (SymbolID c : callees) {
            auto it = index_.find(c);
            if (it != index_.end()) out.push_back(it->second);
        }
        // Sort + unique so iteration order is stable and edges aren't double
        // counted (a caller may reference the same callee several times).
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    tarjan_scc();
}

// Iterative Tarjan. Emits components in reverse-topological order; we assign
// component ids 0,1,2,... in emission order, so an edge u->v in the condensation
// always has comp_[u] > comp_[v].
void CallGraph::tarjan_scc() {
    const int n = static_cast<int>(ids_.size());
    comp_.assign(n, -1);
    std::vector<int> index(n, -1), lowlink(n, 0);
    std::vector<char> on_stack(n, 0);
    std::vector<int> stack;
    stack.reserve(n);
    int next_index = 0;
    n_comps_ = 0;

    // Explicit DFS stack of (node, next-adjacency-cursor).
    std::vector<std::pair<int, int>> dfs;
    dfs.reserve(n);

    for (int root = 0; root < n; ++root) {
        if (index[root] != -1) continue;
        dfs.push_back({root, 0});
        while (!dfs.empty()) {
            auto& [v, ci] = dfs.back();
            if (ci == 0) {
                index[v] = lowlink[v] = next_index++;
                stack.push_back(v);
                on_stack[v] = 1;
            }
            if (ci < static_cast<int>(adj_[v].size())) {
                int w = adj_[v][ci++];
                if (index[w] == -1) {
                    dfs.push_back({w, 0});
                } else if (on_stack[w]) {
                    lowlink[v] = std::min(lowlink[v], index[w]);
                }
            } else {
                // Done with v; if it's an SCC root, pop the component.
                if (lowlink[v] == index[v]) {
                    while (true) {
                        int w = stack.back();
                        stack.pop_back();
                        on_stack[w] = 0;
                        comp_[w] = n_comps_;
                        if (w == v) break;
                    }
                    ++n_comps_;
                }
                dfs.pop_back();
                if (!dfs.empty()) {
                    int parent = dfs.back().first;
                    lowlink[parent] = std::min(lowlink[parent], lowlink[v]);
                }
            }
        }
    }
}

std::vector<std::vector<int>> CallGraph::cycles() const {
    const int n = static_cast<int>(ids_.size());
    std::vector<std::vector<int>> members(n_comps_);
    for (int v = 0; v < n; ++v) members[comp_[v]].push_back(v);

    // Detect self-loops so a single node that calls itself counts as a cycle.
    std::vector<char> self_loop(n_comps_, 0);
    for (int v = 0; v < n; ++v) {
        if (std::binary_search(adj_[v].begin(), adj_[v].end(), v))
            self_loop[comp_[v]] = 1;
    }

    std::vector<std::vector<int>> out;
    for (int c = 0; c < n_comps_; ++c) {
        if (members[c].size() > 1 || self_loop[c]) {
            std::sort(members[c].begin(), members[c].end());
            out.push_back(std::move(members[c]));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.front() < b.front(); });
    return out;
}

std::vector<int> CallGraph::incoming_reach() const {
    const int n = static_cast<int>(ids_.size());
    const int c = n_comps_;
    if (c == 0) return {};

    std::vector<int> comp_size(c, 0);
    for (int v = 0; v < n; ++v) ++comp_size[comp_[v]];

    // Condensation edges P -> S (P != S), deduplicated.
    std::vector<std::vector<int>> cadj(c);
    for (int u = 0; u < n; ++u) {
        for (int w : adj_[u]) {
            if (comp_[u] != comp_[w]) cadj[comp_[u]].push_back(comp_[w]);
        }
    }
    for (auto& e : cadj) {
        std::sort(e.begin(), e.end());
        e.erase(std::unique(e.begin(), e.end()), e.end());
    }

    // ancestors[S] = bitset over components that can reach S (forward paths).
    // Process components in topological order. Tarjan gave reverse-topo ids, so
    // descending id == topological (sources first); pushing each component's
    // ancestor set (plus itself) onto its successors.
    const int words = (c + 63) / 64;
    std::vector<uint64_t> bits(static_cast<size_t>(c) * words, 0);
    auto row = [&](int comp) { return bits.data() + static_cast<size_t>(comp) * words; };

    for (int p = c - 1; p >= 0; --p) {
        uint64_t* ap = row(p);
        for (int s : cadj[p]) {
            uint64_t* as = row(s);
            for (int w = 0; w < words; ++w) as[w] |= ap[w];
            as[p >> 6] |= (uint64_t{1} << (p & 63));  // p itself reaches s
        }
    }

    // reach(node in comp C) = sum of sizes of all ancestor comps + (|C| - 1).
    std::vector<int> comp_reach(c, 0);
    for (int s = 0; s < c; ++s) {
        long total = 0;
        const uint64_t* as = row(s);
        for (int w = 0; w < words; ++w) {
            uint64_t b = as[w];
            while (b) {
                int bit = __builtin_ctzll(b);
                b &= b - 1;
                total += comp_size[w * 64 + bit];
            }
        }
        total += comp_size[s] - 1;  // other members of the same SCC
        comp_reach[s] = static_cast<int>(total);
    }

    std::vector<int> reach(n, 0);
    for (int v = 0; v < n; ++v) reach[v] = comp_reach[comp_[v]];
    return reach;
}

}  // namespace analysis
}  // namespace lci
