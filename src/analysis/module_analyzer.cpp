#include <lci/analysis/module_analyzer.h>

#include <lci/analysis/call_graph.h>
#include <lci/analysis/coupling_analyzer.h>
#include <lci/core/text.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <string>

namespace lci {

namespace {

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// Prefix-based cohesion for a set of symbols.
double prefix_cohesion(const std::vector<const EnhancedSymbol*>& syms) {
    if (syms.empty()) return 0.0;

    absl::flat_hash_map<std::string, int> prefix_counts;
    for (const auto* sym : syms) {
        auto pos = sym->symbol.name.find('_');
        std::string prefix = (pos != std::string::npos)
            ? text::ascii_lower(sym->symbol.name.substr(0, pos))
            : text::ascii_lower(sym->symbol.name);
        prefix_counts[prefix]++;
    }

    int max_count = 0;
    for (const auto& [_, count] : prefix_counts) {
        if (count > max_count) max_count = count;
    }
    return static_cast<double>(max_count) / static_cast<double>(syms.size());
}

double stability_score(int sym_count) {
    if (sym_count == 0) return 0.0;
    return 1.0 / (1.0 + static_cast<double>(sym_count) / 10.0);
}

}  // namespace

// ---------------------------------------------------------------------------
// Module classification
// ---------------------------------------------------------------------------


ModuleAnalysis ModuleAnalyzer::analyze_graph(
    const std::vector<FileSymbolData>& files, std::string_view project_root,
    const std::function<std::vector<SymbolID>(SymbolID)>& callees_of) const {
    if (!callees_of) return analyze(files, project_root);

    // Node set: function-like, non-scaffold symbols of code files; each node
    // remembers its directory (the label space) and file.
    struct NodeMeta {
        std::string dir;
        std::string file;
    };
    std::vector<SymbolID> nodes;
    absl::flat_hash_map<SymbolID, NodeMeta> meta;
    for (const auto& file : files) {
        if (!CouplingAnalyzer::is_code_file(file.path)) continue;
        std::string pkg =
            CouplingAnalyzer::get_package_name(file.path, project_root);
        for (const auto* sym : file.symbols) {
            if (sym == nullptr || sym->symbol.test_scaffold) continue;
            auto t = sym->symbol.type;
            if (t != SymbolType::Function && t != SymbolType::Method &&
                t != SymbolType::Constructor)
                continue;
            nodes.push_back(sym->id);
            meta[sym->id] = {pkg, file.path};
        }
    }

    ModuleAnalysis result;
    result.detection_strategy = "call_graph_louvain";
    if (nodes.empty()) {
        result.metrics.average_coupling = -1.0;
        result.metrics.architectural_score = -1.0;
        return result;
    }

    analysis::CallGraph graph;
    graph.build(nodes, callees_of);
    double modularity = 0.0;
    auto comm = graph.louvain_communities(modularity);
    result.modularity = modularity;

    int k = 0;
    for (int c : comm) k = std::max(k, c + 1);
    std::vector<std::vector<int>> members(k);
    for (int i = 0; i < static_cast<int>(comm.size()); ++i)
        members[comm[i]].push_back(i);

    // Directed edge counts between communities feed cohesion (internal edge
    // share), external_deps (distinct partner modules), and Martin
    // instability (efferent / (afferent + efferent)).
    std::vector<int> internal_edges(k, 0);
    std::vector<absl::flat_hash_map<int, int>> out_edges(k);  // c -> {c2: n}
    std::vector<int> afferent(k, 0), efferent(k, 0);
    const auto& adj = graph.adjacency();
    for (int u = 0; u < graph.node_count(); ++u) {
        for (int v : adj[u]) {
            int cu = comm[u], cv = comm[v];
            if (cu == cv) {
                ++internal_edges[cu];
            } else {
                ++out_edges[cu][cv];
                ++efferent[cu];
                ++afferent[cv];
            }
        }
    }

    // dir -> (community -> member count), for the split-directory signal.
    absl::flat_hash_map<std::string, absl::flat_hash_map<int, int>> dir_comms;

    struct Built {
        ModuleBoundary mb;
        int size;
    };
    std::vector<Built> built;
    for (int c = 0; c < k; ++c) {
        const auto& mem = members[c];
        if (static_cast<int>(mem.size()) < 2) continue;  // singletons: not modules

        // Directory histogram + distinct files.
        absl::flat_hash_map<std::string, int> dirs;
        absl::flat_hash_set<std::string> file_set;
        for (int idx : mem) {
            const NodeMeta& m = meta[graph.id_at(idx)];
            ++dirs[m.dir];
            file_set.insert(m.file);
        }
        // Majority dir first, then count desc / name asc (deterministic).
        std::vector<std::pair<std::string, int>> ranked(dirs.begin(),
                                                        dirs.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto& a,
                                                   const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });

        ModuleBoundary mb;
        mb.name = ranked.front().first;
        mb.path = mb.name;
        mb.type = classify_module_by_path(mb.name);
        for (const auto& [d, n] : ranked) mb.spanned_dirs.push_back(d);
        mb.dir_purity = static_cast<double>(ranked.front().second) /
                        static_cast<double>(mem.size());
        mb.file_count = static_cast<int>(file_set.size());
        mb.function_count = static_cast<int>(mem.size());
        int internal = internal_edges[c];
        int external = efferent[c] + afferent[c];
        int incident = internal + external;
        mb.cohesion_score =
            incident > 0
                ? static_cast<double>(internal) / static_cast<double>(incident)
                : 1.0;
        mb.coupling_score =
            incident > 0
                ? static_cast<double>(external) / static_cast<double>(incident)
                : 0.0;
        mb.external_deps = static_cast<int>(out_edges[c].size());
        for (const auto& [c2, n] : out_edges[c]) {
            (void)c2;
            (void)n;
        }
        mb.stability = external > 0 ? static_cast<double>(efferent[c]) /
                                          static_cast<double>(external)
                                    : 0.0;
        built.push_back({std::move(mb), static_cast<int>(mem.size())});

        for (const auto& [d, n] : dirs) dir_comms[d][c] += n;
    }

    // No community of module size (edgeless or tiny corpus): the graph has
    // nothing to say — fall back to directory grouping rather than emitting
    // an empty section.
    if (built.empty()) return analyze(files, project_root);

    // Deterministic order: member count desc, then name asc.
    std::sort(built.begin(), built.end(), [](const Built& a, const Built& b) {
        if (a.size != b.size) return a.size > b.size;
        return a.mb.name < b.mb.name;
    });
    // Several communities can share a majority directory; disambiguate the
    // display name positionally so rows stay addressable (dir#2, dir#3...).
    absl::flat_hash_map<std::string, int> name_seen;
    for (auto& b : built) {
        int n = ++name_seen[b.mb.name];
        if (n > 1) b.mb.name += "#" + std::to_string(n);
    }
    for (auto& b : built) {
        result.module_types[b.mb.type]++;
        result.modules.push_back(std::move(b.mb));
    }

    // A directory is SPLIT when at least two communities each hold a
    // significant share of it (>= 20% or >= 3 members): the layout and the
    // call structure disagree there.
    std::vector<std::string> split;
    for (const auto& [dir, cm] : dir_comms) {
        int total = 0;
        for (const auto& [c, n] : cm) total += n;
        int significant = 0;
        for (const auto& [c, n] : cm) {
            (void)c;
            if (n >= 3 || (total > 0 && 5 * n >= total)) ++significant;
        }
        if (significant >= 2) split.push_back(dir);
    }
    std::sort(split.begin(), split.end());
    result.split_dirs = std::move(split);

    // Aggregates over real per-module values.
    double coh = 0.0, coup = 0.0;
    for (const auto& m : result.modules) {
        coh += m.cohesion_score;
        coup += m.coupling_score;
    }
    int n = static_cast<int>(result.modules.size());
    result.metrics.total_modules = n;
    result.metrics.average_cohesion =
        n > 0 ? coh / static_cast<double>(n) : 0.0;
    result.metrics.average_coupling =
        n > 0 ? coup / static_cast<double>(n) : -1.0;
    // Modularity Q is the partition-quality number this section used to fake
    // with a 0.8 constant; report the real thing in its place.
    result.metrics.architectural_score = n > 0 ? modularity : -1.0;
    return result;
}

std::string ModuleAnalyzer::classify_module_by_path(std::string_view path) {
    std::string lower = text::ascii_lower(path);

    if (contains(lower, "api") || contains(lower, "controller") ||
        contains(lower, "handler"))
        return "API Layer";
    if (contains(lower, "service") || contains(lower, "business") ||
        contains(lower, "logic"))
        return "Service Layer";
    if (contains(lower, "model") || contains(lower, "entity") ||
        contains(lower, "data"))
        return "Data Layer";
    if (contains(lower, "repository") || contains(lower, "dao"))
        return "Repository Layer";
    if (contains(lower, "util") || contains(lower, "helper"))
        return "Utility";
    if (contains(lower, "test") || contains(lower, "spec"))
        return "Test";
    if (contains(lower, "config") || contains(lower, "setting"))
        return "Configuration";
    if (contains(lower, "middleware") || contains(lower, "filter"))
        return "Middleware";
    // Broadened from the original five-bucket set: a four-repo field run
    // (2026-08-26) showed every module of every corpus reporting "General",
    // which made the type column dead weight.
    if (contains(lower, "cmd") || contains(lower, "entrypoint"))
        return "Entry Point";
    if (contains(lower, "migration") || contains(lower, "storage") ||
        contains(lower, "store") || contains(lower, "database"))
        return "Data Layer";
    if (contains(lower, "endpoint") || contains(lower, "route") ||
        contains(lower, "http") || contains(lower, "server") ||
        contains(lower, "rpc"))
        return "API Layer";
    if (contains(lower, "website") || contains(lower, "frontend") ||
        contains(lower, "webapp") || contains(lower, "/ui") ||
        lower.rfind("ui/", 0) == 0 || contains(lower, "pages") ||
        contains(lower, "components") || contains(lower, "views"))
        return "UI";
    if (contains(lower, "parser") || contains(lower, "lexer") ||
        contains(lower, "compil") || contains(lower, "interpret") ||
        contains(lower, "evaluat") || contains(lower, "grammar"))
        return "Language Core";
    if (contains(lower, "auth"))
        return "Auth";
    if (contains(lower, "docs") || contains(lower, "doc/"))
        return "Docs";
    if (contains(lower, "script") || contains(lower, "tool"))
        return "Tooling";

    return "General";
}

// ---------------------------------------------------------------------------
// Main analysis
// ---------------------------------------------------------------------------

ModuleAnalysis ModuleAnalyzer::analyze(
    const std::vector<FileSymbolData>& files,
    std::string_view project_root) const {

    // Group symbols by package = directory relative to project_root
    // ("(root)" for root-level files), via getPackageName. Both the NAME and
    // the TYPE classification use this stable, repo-relative package path —
    // NOT the absolute directory. Go's ModuleAnalysis builder instead names
    // by basename and classifies on the absolute path, which misclassifies
    // (e.g. any module under a ".../tests/..." checkout path becomes "Test")
    // and disagrees with Go's own repository-map naming; that asymmetry is a
    // Go bug the C++ port deliberately does not replicate. Only code files
    // participate; function_count counts functions/methods.
    absl::flat_hash_map<std::string, std::vector<const EnhancedSymbol*>> pkg_syms;
    absl::flat_hash_map<std::string, absl::flat_hash_map<std::string, bool>> pkg_files;
    absl::flat_hash_map<std::string, int> pkg_func_count;

    for (const auto& file : files) {
        if (!CouplingAnalyzer::is_code_file(file.path)) continue;
        std::string pkg =
            CouplingAnalyzer::get_package_name(file.path, project_root);
        for (const auto* sym : file.symbols) {
            pkg_syms[pkg].push_back(sym);
            pkg_files[pkg][file.path] = true;
            if (sym->symbol.type == SymbolType::Function ||
                sym->symbol.type == SymbolType::Method) {
                pkg_func_count[pkg]++;
            }
        }
    }

    // Build module boundaries.
    std::vector<ModuleBoundary> modules;
    for (auto& [pkg, syms] : pkg_syms) {
        ModuleBoundary mb;
        mb.name = pkg;
        mb.type = classify_module_by_path(pkg);
        mb.path = pkg;
        mb.cohesion_score = prefix_cohesion(syms);
        // This analyzer computes NO per-module coupling; the Go port carried
        // a 0.3 constant here that rendered as a real number in detailed
        // sub=modules while overview showed the true CouplingAnalyzer value.
        // -1 = unknown; emitters print n/a, the overview path overwrites
        // with the real number when it has one.
        mb.coupling_score = -1.0;
        mb.stability = stability_score(static_cast<int>(syms.size()));
        mb.file_count = static_cast<int>(pkg_files[pkg].size());
        mb.function_count = pkg_func_count[pkg];
        modules.push_back(std::move(mb));
    }

    // Deterministic order: file_count desc, then name asc. Go relies on map
    // iteration order here (non-deterministic on ties); the C++ port sorts.
    std::sort(modules.begin(), modules.end(),
              [](const ModuleBoundary& a, const ModuleBoundary& b) {
                  if (a.file_count != b.file_count)
                      return a.file_count > b.file_count;
                  return a.name < b.name;
              });

    // Calculate aggregate metrics. Coupling and architectural score are not
    // computed here: -1 = unknown (see coupling_score above).
    double total_cohesion = 0.0;
    for (const auto& m : modules) {
        total_cohesion += m.cohesion_score;
    }

    int count = static_cast<int>(modules.size());
    ModuleAnalysisMetrics metrics;
    metrics.total_modules = count;
    metrics.average_cohesion = (count > 0)
        ? total_cohesion / static_cast<double>(count) : 0.0;
    metrics.average_coupling = -1.0;
    metrics.architectural_score = -1.0;

    ModuleAnalysis result;
    result.modules = std::move(modules);
    result.detection_strategy = "directory_structure";
    result.metrics = metrics;

    return result;
}

}  // namespace lci
