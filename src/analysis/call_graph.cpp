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

// ---------------------------------------------------------------------------
// Louvain community detection
// ---------------------------------------------------------------------------

namespace {

// A weighted undirected graph level used by the Louvain aggregation passes.
struct Level {
    int n{};
    std::vector<std::vector<std::pair<int, double>>> adj;  // neighbor, weight
    std::vector<double> self;  // intra-node (self-loop) weight
    double m2{};               // 2m = sum of all node degrees
    std::vector<double> degree;
};

// One local-moving phase. Fills `comm` (community per node); returns true if any
// node moved (i.e. the partition improved). Deterministic: fixed node order,
// lowest-community-id tie-break.
bool louvain_one_level(const Level& g, std::vector<int>& comm) {
    const int n = g.n;
    comm.resize(n);
    std::vector<double> tot(n, 0.0);  // sum of degrees in community
    for (int i = 0; i < n; ++i) {
        comm[i] = i;
        tot[i] = g.degree[i];
    }
    if (g.m2 <= 0.0) return false;

    bool any_move = false;
    bool improved = true;
    // Scratch: weight from current node to each candidate community.
    std::vector<double> w_to(n, 0.0);
    std::vector<int> touched;
    touched.reserve(16);

    while (improved) {
        improved = false;
        for (int i = 0; i < n; ++i) {
            int ci = comm[i];
            // Accumulate edge weight from i to each neighboring community.
            for (int idx : touched) w_to[idx] = 0.0;
            touched.clear();
            for (const auto& [j, w] : g.adj[i]) {
                int cj = comm[j];
                if (w_to[cj] == 0.0) touched.push_back(cj);
                w_to[cj] += w;
            }
            // Remove i from its community.
            tot[ci] -= g.degree[i];
            double w_to_ci = 0.0;
            for (int idx : touched)
                if (idx == ci) w_to_ci = w_to[idx];

            // Pick the community maximizing modularity gain. Baseline = staying
            // in ci. Gain(c) ∝ w_to[c] - tot[c]*k_i/m2 (constants drop out).
            int best = ci;
            double best_gain = w_to_ci - tot[ci] * g.degree[i] / g.m2;
            for (int c : touched) {
                double gain = w_to[c] - tot[c] * g.degree[i] / g.m2;
                if (gain > best_gain + 1e-12 ||
                    (gain > best_gain - 1e-12 && c < best)) {
                    best_gain = gain;
                    best = c;
                }
            }
            tot[best] += g.degree[i];
            if (best != ci) {
                comm[i] = best;
                improved = true;
                any_move = true;
            }
        }
    }
    return any_move;
}

// Relabels communities to a dense 0..K-1 range (in ascending original id) and
// builds the aggregated next-level graph. Returns the per-node community label.
std::vector<int> relabel(const std::vector<int>& comm, int& k_out) {
    absl::flat_hash_map<int, int> remap;
    std::vector<int> dense(comm.size());
    // Deterministic dense ids: assign in ascending community id.
    std::vector<int> uniq(comm.begin(), comm.end());
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    for (int i = 0; i < static_cast<int>(uniq.size()); ++i) remap[uniq[i]] = i;
    for (size_t i = 0; i < comm.size(); ++i) dense[i] = remap[comm[i]];
    k_out = static_cast<int>(uniq.size());
    return dense;
}

Level aggregate(const Level& g, const std::vector<int>& comm, int k) {
    Level out;
    out.n = k;
    out.adj.assign(k, {});
    out.self.assign(k, 0.0);
    out.degree.assign(k, 0.0);
    // Sum intra/inter community weights.
    std::vector<absl::flat_hash_map<int, double>> w(k);
    for (int i = 0; i < g.n; ++i) {
        int ci = comm[i];
        out.self[ci] += g.self[i];
        for (const auto& [j, wt] : g.adj[i]) {
            int cj = comm[j];
            if (ci == cj) {
                out.self[ci] += wt / 2.0;  // each intra edge seen from both ends
            } else {
                w[ci][cj] += wt;
            }
        }
    }
    for (int c = 0; c < k; ++c) {
        for (auto& [d, wt] : w[c]) out.adj[c].push_back({d, wt});
        std::sort(out.adj[c].begin(), out.adj[c].end());
    }
    out.m2 = g.m2;  // total weight is preserved across aggregation
    for (int c = 0; c < k; ++c) {
        double deg = 2.0 * out.self[c];
        for (const auto& [d, wt] : out.adj[c]) deg += wt;
        out.degree[c] = deg;
    }
    return out;
}

double modularity(const Level& g, const std::vector<int>& comm, int k) {
    if (g.m2 <= 0.0) return 0.0;
    std::vector<double> in(k, 0.0), tot(k, 0.0);
    for (int i = 0; i < g.n; ++i) {
        tot[comm[i]] += g.degree[i];
        in[comm[i]] += 2.0 * g.self[i];
        for (const auto& [j, w] : g.adj[i])
            if (comm[i] == comm[j]) in[comm[i]] += w;  // counted from both ends
    }
    double q = 0.0;
    for (int c = 0; c < k; ++c) {
        double t = tot[c] / g.m2;
        q += in[c] / g.m2 - t * t;
    }
    return q;
}

}  // namespace

std::vector<int> CallGraph::louvain_communities(double& modularity_out) const {
    const int n = static_cast<int>(ids_.size());
    modularity_out = 0.0;
    if (n == 0) return {};

    // Level 0: undirected weighted graph from the directed call edges. Each
    // directed edge contributes weight 1 to the symmetric pair; multi-edges sum.
    Level g0;
    g0.n = n;
    g0.adj.assign(n, {});
    g0.self.assign(n, 0.0);
    g0.degree.assign(n, 0.0);
    std::vector<absl::flat_hash_map<int, double>> w(n);
    for (int u = 0; u < n; ++u) {
        for (int v : adj_[u]) {
            if (u == v) { g0.self[u] += 1.0; continue; }
            w[u][v] += 1.0;
            w[v][u] += 1.0;
        }
    }
    double total = 0.0;
    for (int u = 0; u < n; ++u) {
        for (auto& [v, wt] : w[u]) g0.adj[u].push_back({v, wt});
        std::sort(g0.adj[u].begin(), g0.adj[u].end());
        double deg = 2.0 * g0.self[u];
        for (auto& [v, wt] : g0.adj[u]) deg += wt;
        g0.degree[u] = deg;
        total += deg;
    }
    g0.m2 = total;
    if (g0.m2 <= 0.0) {
        // Edgeless: every node is its own singleton community, Q = 0.
        std::vector<int> singletons(n);
        for (int i = 0; i < n; ++i) singletons[i] = i;
        return singletons;
    }

    // Multilevel loop: local moving, then aggregate, until no improvement.
    // node_comm[i] tracks which current-level super-node original node i is in.
    std::vector<int> node_comm(n);
    for (int i = 0; i < n; ++i) node_comm[i] = i;
    Level cur = g0;

    while (true) {
        std::vector<int> comm;
        bool moved = louvain_one_level(cur, comm);
        int k = 0;
        std::vector<int> dense = relabel(comm, k);
        // Fold this level's assignment into the original-node labeling:
        // each original node follows its current super-node into the new label.
        for (int i = 0; i < n; ++i) node_comm[i] = dense[node_comm[i]];
        if (!moved || k == cur.n) break;
        cur = aggregate(cur, dense, k);
    }

    // Recompute final modularity on the base graph against the folded labels.
    int kf = 0;
    std::vector<int> final_labels = relabel(node_comm, kf);
    modularity_out = modularity(g0, final_labels, kf);
    return final_labels;
}

}  // namespace analysis
}  // namespace lci
