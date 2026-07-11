#include <lci/analysis/feature_analyzer.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/analysis/call_graph.h>
#include <lci/core/text.h>

namespace lci {

namespace {

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

bool contains_any(std::string_view s, const std::vector<std::string_view>& subs) {
    for (auto sub : subs) {
        if (contains(s, sub)) return true;
    }
    return false;
}

bool is_function_like(SymbolType t) {
    return t == SymbolType::Function || t == SymbolType::Method ||
           t == SymbolType::Constructor;
}

bool is_class_like(SymbolType t) {
    return t == SymbolType::Class || t == SymbolType::Struct;
}

// Parent-directory base name of a file path, e.g. "src/auth/login.go" -> "auth".
std::string dir_base(const std::string& path) {
    return std::filesystem::path(path).parent_path().filename().string();
}

}  // namespace

// ---------------------------------------------------------------------------
// Component / feature classification
// ---------------------------------------------------------------------------

std::string FeatureAnalyzer::classify_component_type(const EnhancedSymbol& sym) {
    std::string name = text::ascii_lower(sym.symbol.name);

    if (is_function_like(sym.symbol.type)) {
        if (contains(name, "handler") || contains(name, "controller"))
            return "Controller";
        if (contains(name, "service") || contains(name, "manager"))
            return "Service";
        if (contains(name, "repository") || contains(name, "dao"))
            return "Repository";
        if (contains(name, "model") || contains(name, "entity"))
            return "Model";
        if (contains(name, "util") || contains(name, "helper"))
            return "Utility";
        return "Function";
    }
    if (is_class_like(sym.symbol.type)) {
        if (contains(name, "service")) return "Service";
        if (contains(name, "controller")) return "Controller";
        if (contains(name, "model")) return "Model";
        if (contains(name, "repository")) return "Repository";
        return "Class";
    }
    if (sym.symbol.type == SymbolType::Interface) return "Interface";
    if (sym.symbol.type == SymbolType::Variable) return "Variable";
    if (sym.symbol.type == SymbolType::Constant) return "Constant";

    return "Symbol";
}

std::string FeatureAnalyzer::classify_feature_type(std::string_view name) {
    std::string lower = text::ascii_lower(name);

    if (contains_any(lower, {"user", "auth", "login", "register", "account", "profile"}))
        return "User Management";
    if (contains_any(lower, {"order", "cart", "checkout", "payment", "billing", "invoice"}))
        return "E-commerce";
    if (contains_any(lower, {"product", "catalog", "inventory", "stock", "price"}))
        return "Product Management";
    if (contains_any(lower, {"notification", "email", "sms", "push", "alert"}))
        return "Communication";
    if (contains_any(lower, {"report", "analytics", "dashboard", "metric", "statistic"}))
        return "Reporting";
    if (contains_any(lower, {"search", "filter", "query", "sort"}))
        return "Search";
    if (contains_any(lower, {"upload", "download", "file", "image", "document"}))
        return "File Management";
    if (contains_any(lower, {"config", "setting", "preference", "option"}))
        return "Configuration";
    if (contains_any(lower, {"api", "endpoint", "service", "controller"}))
        return "API";
    if (contains_any(lower, {"database", "db", "sql", "cache", "session"}))
        return "Data Management";

    return "General Feature";
}

// ---------------------------------------------------------------------------
// Main analysis — Louvain community detection over the reference graph.
// ---------------------------------------------------------------------------

FeatureAnalysis FeatureAnalyzer::analyze(
    const std::vector<FileSymbolData>& files,
    const std::function<std::vector<SymbolID>(SymbolID)>& callees_of) const {

    FeatureAnalysis result;

    // 1. Node set: callable symbols (the reference graph is a call graph, so
    //    only call-bearing symbols carry edges). Keep per-id metadata.
    struct NodeMeta {
        const EnhancedSymbol* sym{};
        std::string path;
    };
    std::vector<SymbolID> nodes;
    absl::flat_hash_map<SymbolID, NodeMeta> meta;
    for (const auto& f : files) {
        for (const auto* sym : f.symbols) {
            if (!is_function_like(sym->symbol.type)) continue;
            if (sym->id == 0) continue;  // un-tracked symbol carries no edges
            if (meta.contains(sym->id)) continue;
            nodes.push_back(sym->id);
            meta[sym->id] = NodeMeta{sym, f.path};
        }
    }
    if (nodes.empty()) return result;

    // 2. Build the call graph and partition it into communities (Louvain).
    analysis::CallGraph graph;
    graph.build(nodes, callees_of);
    double modularity = 0.0;
    auto comm = graph.louvain_communities(modularity);
    const int n = graph.node_count();
    const auto& adj = graph.adjacency();

    auto meta_at = [&](int idx) -> const NodeMeta& {
        return meta[graph.id_at(idx)];
    };

    // 3. Group node indices by community id. Community count <= n, so reserve
    //    the community-keyed maps to n up front (no rehash in the edge loop).
    absl::flat_hash_map<int, std::vector<int>> members;
    members.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) members[comm[i]].push_back(i);

    // 4. Edge accounting: per-community internal/boundary counts (for cohesion)
    //    and directed inter-community edge counts (for cross-feature deps).
    absl::flat_hash_map<int, long> internal_edges;   // both endpoints in c
    absl::flat_hash_map<int, long> boundary_edges;    // exactly one endpoint in c
    absl::flat_hash_map<int, long> out_cross_edges;   // edges leaving c
    absl::flat_hash_map<long long, long> pair_edges;  // (ca,cb) -> directed count
    absl::flat_hash_map<int, int> intra_degree;       // node idx -> intra edges
    internal_edges.reserve(static_cast<size_t>(n));
    boundary_edges.reserve(static_cast<size_t>(n));
    out_cross_edges.reserve(static_cast<size_t>(n));
    intra_degree.reserve(static_cast<size_t>(n));
    for (int u = 0; u < n; ++u) {
        int cu = comm[u];
        for (int v : adj[u]) {
            int cv = comm[v];
            if (cu == cv) {
                internal_edges[cu]++;
                intra_degree[u]++;
                intra_degree[v]++;
            } else {
                boundary_edges[cu]++;
                boundary_edges[cv]++;
                out_cross_edges[cu]++;
                pair_edges[static_cast<long long>(cu) * 1000000007LL + cv]++;
            }
        }
    }

    // 5. Build features (communities with >= 2 members) and orphans (isolated
    //    singletons that cannot be graph-clustered).
    auto component_for = [&](int idx) {
        const auto& m = meta_at(idx);
        ComponentInfo ci;
        ci.name = m.sym->symbol.name;
        ci.type = classify_component_type(*m.sym);
        ci.location = m.path + ":" + std::to_string(m.sym->symbol.line);
        ci.complexity = static_cast<double>(m.sym->complexity);
        for (int v : adj[idx]) ci.dependencies.push_back(meta_at(v).sym->symbol.name);
        std::sort(ci.dependencies.begin(), ci.dependencies.end());
        return ci;
    };

    // Stable community iteration order.
    std::vector<int> comm_ids;
    comm_ids.reserve(members.size());
    for (const auto& [cid, _] : members) comm_ids.push_back(cid);
    std::sort(comm_ids.begin(), comm_ids.end());

    absl::flat_hash_map<int, std::string> feature_name;  // community id -> name
    double total_cohesion = 0.0;
    double total_complexity = 0.0;

    for (int cid : comm_ids) {
        auto& idxs = members[cid];
        if (idxs.size() < 2) {
            // Isolated symbol: not a feature.
            result.orphan_components.push_back(component_for(idxs.front()));
            continue;
        }

        // Representative = highest intra-community degree (the cluster hub),
        // tie-broken by name then SymbolID for a TOTAL order — it names the
        // feature, so a partial order would make the feature name (and thus
        // output) nondeterministic when two members share degree+name
        // (cf. the entry-points same-name flake). SymbolID is unique.
        std::sort(idxs.begin(), idxs.end(), [&](int a, int b) {
            int da = intra_degree.count(a) ? intra_degree[a] : 0;
            int db = intra_degree.count(b) ? intra_degree[b] : 0;
            if (da != db) return da > db;
            const auto& na = meta_at(a).sym->symbol.name;
            const auto& nb = meta_at(b).sym->symbol.name;
            if (na != nb) return na < nb;
            return graph.id_at(a) < graph.id_at(b);
        });
        const auto& hub = meta_at(idxs.front());
        std::string base = dir_base(hub.path);
        std::string name = base.empty() ? hub.sym->symbol.name
                                        : base + "/" + hub.sym->symbol.name;
        feature_name[cid] = name;

        Feature feat;
        feat.name = name;
        feat.primary_module = classify_feature_type(name);

        double comp_sum = 0.0;
        std::vector<int> sorted_members = idxs;
        std::sort(sorted_members.begin(), sorted_members.end(), [&](int a, int b) {
            const auto& na = meta_at(a).sym->symbol.name;
            const auto& nb = meta_at(b).sym->symbol.name;
            if (na != nb) return na < nb;
            return graph.id_at(a) < graph.id_at(b);  // total order
        });
        feat.components.reserve(sorted_members.size());
        for (int idx : sorted_members) {
            auto ci = component_for(idx);
            comp_sum += ci.complexity;
            feat.components.push_back(std::move(ci));
        }

        // Real graph cohesion: internal / (internal + boundary) edges in [0,1].
        long in_e = internal_edges.count(cid) ? internal_edges[cid] : 0;
        long bd_e = boundary_edges.count(cid) ? boundary_edges[cid] : 0;
        double cohesion = (in_e + bd_e > 0)
                              ? static_cast<double>(in_e) /
                                    static_cast<double>(in_e + bd_e)
                              : 0.0;
        feat.confidence = cohesion;

        total_cohesion += cohesion;
        total_complexity += feat.components.empty()
                                ? 0.0
                                : comp_sum / static_cast<double>(feat.components.size());

        result.features.push_back(std::move(feat));
    }

    // 6. Cross-feature dependencies from directed inter-community edges.
    for (const auto& [key, count] : pair_edges) {
        int ca = static_cast<int>(key / 1000000007LL);
        int cb = static_cast<int>(key % 1000000007LL);
        auto ita = feature_name.find(ca);
        auto itb = feature_name.find(cb);
        if (ita == feature_name.end() || itb == feature_name.end()) continue;
        // strength = fraction of ca's outgoing cross-community edges that target
        // cb. The denominator counts ALL of ca's cross edges (including any to
        // orphan singletons, which are skipped as dep targets), so a feature's
        // strengths need not sum to 1 — it is a coupling fraction, not a
        // normalized distribution.
        long total_out = out_cross_edges.count(ca) ? out_cross_edges[ca] : 0;
        FeatureDependency dep;
        dep.feature_a = ita->second;
        dep.feature_b = itb->second;
        dep.type = "calls";
        dep.strength = total_out > 0 ? static_cast<double>(count) /
                                           static_cast<double>(total_out)
                                     : 0.0;
        result.cross_feature_deps.push_back(std::move(dep));
    }

    // 7. Deterministic ordering of all outputs (total orders — feature names can
    //    collide across communities, so every comparator has a final tiebreak).
    std::sort(result.features.begin(), result.features.end(),
              [](const Feature& a, const Feature& b) {
                  if (a.components.size() != b.components.size())
                      return a.components.size() > b.components.size();
                  if (a.name != b.name) return a.name < b.name;
                  const std::string& la =
                      a.components.empty() ? a.name : a.components.front().location;
                  const std::string& lb =
                      b.components.empty() ? b.name : b.components.front().location;
                  return la < lb;
              });
    std::sort(result.cross_feature_deps.begin(), result.cross_feature_deps.end(),
              [](const FeatureDependency& a, const FeatureDependency& b) {
                  if (a.feature_a != b.feature_a) return a.feature_a < b.feature_a;
                  if (a.feature_b != b.feature_b) return a.feature_b < b.feature_b;
                  return a.strength > b.strength;
              });
    std::sort(result.orphan_components.begin(), result.orphan_components.end(),
              [](const ComponentInfo& a, const ComponentInfo& b) {
                  if (a.name != b.name) return a.name < b.name;
                  return a.location < b.location;
              });

    int count = static_cast<int>(result.features.size());
    int total_comp = 0;
    for (const auto& f : result.features)
        total_comp += static_cast<int>(f.components.size());
    result.metrics.total_features = count;
    result.metrics.average_components =
        count > 0 ? static_cast<double>(total_comp) / static_cast<double>(count)
                  : 0.0;
    result.metrics.avg_cohesion =
        count > 0 ? total_cohesion / static_cast<double>(count) : 0.0;
    result.metrics.avg_complexity =
        count > 0 ? total_complexity / static_cast<double>(count) : 0.0;

    return result;
}

}  // namespace lci
