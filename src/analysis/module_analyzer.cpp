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

    // Node set: function-like, non-scaffold symbols of code files. The
    // report's rows are the DECLARED modules (directories) — that is how the
    // system is currently oriented — and the call-graph communities are the
    // ACTUAL structure they are compared against.
    struct NodeMeta {
        std::string dir;
        std::string file;
    };
    std::vector<SymbolID> nodes;
    absl::flat_hash_map<SymbolID, NodeMeta> meta;
    // Repo-relative file paths throughout: findings must print the path a
    // reader can open, not the absolute checkout location.
    auto rel_of = [&](std::string_view path) {
        if (!project_root.empty() && path.size() > project_root.size() &&
            path.substr(0, project_root.size()) == project_root) {
            size_t start = project_root.size();
            if (start < path.size() &&
                (path[start] == '/' || path[start] == '\\'))
                ++start;
            return std::string(path.substr(start));
        }
        return std::string(path);
    };
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
            meta[sym->id] = {pkg, rel_of(file.path)};
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

    const int n_nodes = graph.node_count();
    int k = 0;
    for (int c : comm) k = std::max(k, c + 1);

    // Community -> per-directory member histogram. A community is OWNED by a
    // directory when that directory holds a majority of its members; a
    // community no directory owns is SHARED (utility) — spanning it is good
    // structure, never drift.
    std::vector<absl::flat_hash_map<std::string, int>> comm_dirs(k);
    std::vector<int> comm_size(k, 0);
    for (int i = 0; i < n_nodes; ++i) {
        const NodeMeta& m = meta[graph.id_at(i)];
        ++comm_dirs[comm[i]][m.dir];
        ++comm_size[comm[i]];
    }
    // Owner dir per community ("" = shared/utility). Ties break
    // lexicographically for determinism.
    std::vector<std::string> comm_owner(k);
    for (int c = 0; c < k; ++c) {
        std::string best;
        int best_n = 0;
        for (const auto& [d, n] : comm_dirs[c]) {
            if (n > best_n || (n == best_n && d < best)) {
                best = d;
                best_n = n;
            }
        }
        if (comm_size[c] >= 2 && 2 * best_n > comm_size[c])
            comm_owner[c] = best;
    }

    // Directory-level structures: members, dominant community, edges.
    struct DirStat {
        std::vector<int> members;                 // node indices
        absl::flat_hash_map<int, int> comms;      // community -> members
        int internal_edges{};
        int efferent{}, afferent{};
        absl::flat_hash_map<std::string, int> out_dirs;  // partner -> edges
        // partner dir -> distinct callee symbols hit (interface width).
        absl::flat_hash_map<std::string, absl::flat_hash_set<SymbolID>>
            out_targets;
    };
    absl::flat_hash_map<std::string, DirStat> dirs;
    for (int i = 0; i < n_nodes; ++i) {
        const NodeMeta& m = meta[graph.id_at(i)];
        auto& ds = dirs[m.dir];
        ds.members.push_back(i);
        ++ds.comms[comm[i]];
    }
    const auto& adj = graph.adjacency();
    for (int u = 0; u < n_nodes; ++u) {
        const std::string& du = meta[graph.id_at(u)].dir;
        for (int v : adj[u]) {
            const std::string& dv = meta[graph.id_at(v)].dir;
            if (du == dv) {
                ++dirs[du].internal_edges;
            } else {
                auto& src = dirs[du];
                ++src.efferent;
                ++src.out_dirs[dv];
                src.out_targets[dv].insert(graph.id_at(v));
                ++dirs[dv].afferent;
            }
        }
    }

    // Rows: one per directory. cohesion = the LABEL cohesion — the share of
    // the directory's members in its dominant community (how well "current"
    // matches "actual"); coupling = external share of its call edges;
    // stability = Martin instability over directory edges.
    struct Built {
        ModuleBoundary mb;
        int size;
    };
    std::vector<Built> built;
    for (auto& [dir, ds] : dirs) {
        int size = static_cast<int>(ds.members.size());
        if (size < 1) continue;
        int dom_comm = -1, dom_n = 0;
        for (const auto& [c, n] : ds.comms) {
            if (n > dom_n || (n == dom_n && c < dom_comm)) {
                dom_comm = c;
                dom_n = n;
            }
        }
        absl::flat_hash_set<std::string> file_set;
        for (int idx : ds.members) file_set.insert(meta[graph.id_at(idx)].file);

        ModuleBoundary mb;
        mb.name = dir;
        mb.path = dir;
        mb.type = classify_module_by_path(dir);
        mb.cohesion_score =
            static_cast<double>(dom_n) / static_cast<double>(size);
        mb.dir_purity = mb.cohesion_score;
        int external = ds.efferent + ds.afferent;
        int incident = ds.internal_edges + external;
        mb.coupling_score =
            incident > 0 ? static_cast<double>(external) /
                               static_cast<double>(incident)
                         : 0.0;
        mb.external_deps = static_cast<int>(ds.out_dirs.size());
        mb.stability = external > 0 ? static_cast<double>(ds.efferent) /
                                          static_cast<double>(external)
                                    : 0.0;
        mb.file_count = static_cast<int>(file_set.size());
        mb.function_count = size;
        // Actual structure the label maps to: number of communities with a
        // significant share of this directory (>= 3 members or >= 20%).
        int significant = 0;
        for (const auto& [c, cn] : ds.comms) {
            (void)c;
            if (cn >= 3 || 5 * cn >= size) ++significant;
        }
        // spanned_dirs is repurposed as a count carrier via size (emitters
        // read communities from it): store nothing here; the emitter uses
        // external_deps/cohesion. Keep the significant-community count in
        // stability? No — add to split_dirs below instead.
        if (significant >= 2) result.split_dirs.push_back(dir);
        built.push_back({std::move(mb), size});
    }
    std::sort(built.begin(), built.end(), [](const Built& a, const Built& b) {
        if (a.size != b.size) return a.size > b.size;
        return a.mb.name < b.mb.name;
    });
    std::sort(result.split_dirs.begin(), result.split_dirs.end());
    for (auto& b : built) {
        result.module_types[b.mb.type]++;
        result.modules.push_back(std::move(b.mb));
    }
    if (result.modules.empty()) return analyze(files, project_root);

    // Misplaced files: a file whose graph members predominantly join a
    // community OWNED by a different directory. Shared/utility communities
    // (no owner) are exempt. Evidence bar: majority of the file's members
    // and at least 2 symbols, or a single-symbol file entirely elsewhere.
    absl::flat_hash_map<std::string,
                        std::pair<std::string, absl::flat_hash_map<int, int>>>
        file_comms;  // file -> (home dir, community histogram)
    for (int i = 0; i < n_nodes; ++i) {
        const NodeMeta& m = meta[graph.id_at(i)];
        auto& fc = file_comms[m.file];
        fc.first = m.dir;
        ++fc.second[comm[i]];
    }
    for (const auto& [file, fc] : file_comms) {
        const std::string& home = fc.first;
        int total = 0, dom_comm = -1, dom_n = 0;
        for (const auto& [c, n] : fc.second) {
            total += n;
            if (n > dom_n || (n == dom_n && c < dom_comm)) {
                dom_comm = c;
                dom_n = n;
            }
        }
        if (dom_comm < 0 || 2 * dom_n <= total) continue;  // no majority
        const std::string& owner = comm_owner[dom_comm];
        if (owner.empty() || owner == home) continue;  // shared, or in place
        result.misplaced_files.push_back(
            {file, home, owner, dom_n, total});
    }
    std::sort(result.misplaced_files.begin(), result.misplaced_files.end(),
              [](const MisplacedFile& a, const MisplacedFile& b) {
                  if (a.symbols != b.symbols) return a.symbols > b.symbols;
                  return a.file < b.file;
              });

    // Tight coupling: a directory pair where the caller reaches MANY distinct
    // symbols of the callee. Few targets = interface-like, never listed.
    constexpr int kWideInterface = 5;
    for (const auto& [dir, ds] : dirs) {
        for (const auto& [callee, targets] : ds.out_targets) {
            int width = static_cast<int>(targets.size());
            if (width < kWideInterface) continue;
            result.tight_couplings.push_back(
                {dir, callee, ds.out_dirs.at(callee), width});
        }
    }
    std::sort(result.tight_couplings.begin(), result.tight_couplings.end(),
              [](const TightCoupling& a, const TightCoupling& b) {
                  if (a.distinct_targets != b.distinct_targets)
                      return a.distinct_targets > b.distinct_targets;
                  if (a.caller != b.caller) return a.caller < b.caller;
                  return a.callee < b.callee;
              });

    // The measured communities, as the refactoring guide: what the modules
    // WOULD look like if the layout followed the call structure. Sorted by
    // size desc, label asc; singletons omitted.
    for (int c = 0; c < k; ++c) {
        if (comm_size[c] < 2) continue;
        CommunitySummary cs;
        cs.label = comm_owner[c].empty() ? "(shared)" : comm_owner[c];
        cs.size = comm_size[c];
        absl::flat_hash_set<std::string> cfiles;
        for (int i = 0; i < n_nodes; ++i) {
            if (comm[i] != c) continue;
            cfiles.insert(meta[graph.id_at(i)].file);
        }
        cs.files = static_cast<int>(cfiles.size());
        std::vector<std::pair<std::string, int>> ranked(comm_dirs[c].begin(),
                                                        comm_dirs[c].end());
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second != b.second) return a.second > b.second;
                      return a.first < b.first;
                  });
        for (const auto& [d, dn] : ranked) {
            (void)dn;
            cs.dirs.push_back(d);
        }
        result.communities.push_back(std::move(cs));
    }
    std::sort(result.communities.begin(), result.communities.end(),
              [](const CommunitySummary& a, const CommunitySummary& b) {
                  if (a.size != b.size) return a.size > b.size;
                  return a.label < b.label;
              });

    double coh = 0.0, coup = 0.0;
    for (const auto& m : result.modules) {
        coh += m.cohesion_score;
        coup += m.coupling_score;
    }
    int n = static_cast<int>(result.modules.size());
    result.metrics.total_modules = n;
    result.metrics.average_cohesion = coh / static_cast<double>(n);
    result.metrics.average_coupling = coup / static_cast<double>(n);
    result.metrics.architectural_score = modularity;
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
