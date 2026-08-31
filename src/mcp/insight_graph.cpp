#include <lci/mcp/insight_sections.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include <lci/analysis/coupling_analyzer.h>
#include <lci/core/graph_propagator.h>
#include <lci/core/reference_tracker.h>
#include <lci/git/analyzer.h>
#include <lci/git/provider.h>
#include <lci/indexing/master_index.h>
#include <lci/language_map.h>
#include <lci/search/search_engine.h>
#include <lci/symbol.h>
#include <lci/version.h>

#include <absl/container/flat_hash_set.h>

#include <lci/analysis/call_graph.h>
#include <lci/analysis/layer_analyzer.h>

namespace lci {
namespace mcp {
namespace insight {

// Canonical top-to-bottom depth of the architectural layers LayerAnalyzer
// classifies into. Calls should flow downward (shallow -> deep). Utility is
// cross-cutting and unknown layers are unranked — both exempt (return -1).
// Middleware/handler chain dispatch is the definitional shape of the pattern
// (a middleware MUST call the next handler), not an architecture violation —
// whitelist those call edges before flagging upward calls.
bool is_middleware_chain_call(std::string_view caller, std::string_view callee) {
    auto contains_ci = [](std::string_view hay, std::string_view needle) {
        if (hay.size() < needle.size()) return false;
        for (size_t i = 0; i <= hay.size() - needle.size(); ++i) {
            size_t j = 0;
            while (j < needle.size() &&
                   std::tolower(static_cast<unsigned char>(hay[i + j])) ==
                       needle[j])
                ++j;
            if (j == needle.size()) return true;
        }
        return false;
    };
    if (contains_ci(caller, "middleware") || contains_ci(callee, "middleware"))
        return true;
    // Calling into a handler (or the `next` link of a chain) is dispatch.
    if (contains_ci(callee, "handle")) return true;
    if (callee.size() >= 4) {
        std::string_view tail = callee.substr(callee.size() - 4);
        if (contains_ci(tail, "next")) return true;
    }
    return false;
}

int layer_depth(const std::string& layer) {
    if (layer == "Presentation Layer") return 0;
    if (layer == "Application Layer") return 1;
    if (layer == "Domain Layer") return 2;
    if (layer == "Data Layer") return 3;
    return -1;
}

// Single build of analysis::CallGraph over the real call graph, yielding three
// signals at once (Karpathy: don't rebuild the graph three times):
//   - load-bearing: exact transitive-caller reach (SCC + bitset closure),
//   - cycles: strongly-connected components = circular dependencies,
//   - clusters: Louvain communities + modularity Q (real graph clustering that
//     supersedes directory/name-prefix module heuristics).
// Every call edge is weighted 1.0 — an edge carries dependence or it doesn't;
// no fixed per-hop decay constant. (Principled edge weighting would come from
// per-call-site control flow — loops/branches — not a constant; not surfaced
// per-edge yet.)
GraphSignals compute_graph_signals(const MasterIndex& indexer,
                                   const std::vector<FileSymbolData>& files,
                                   std::string_view project_root, size_t top_n,
                                   const GraphPropagator* propagator) {
    const auto& ref = indexer.ref_tracker();

    std::vector<SymbolID> nodes;
    absl::flat_hash_map<SymbolID, std::pair<std::string, std::string>> meta;
    absl::flat_hash_map<SymbolID, std::string> layer;  // id -> architectural layer
    for (const auto& f : files) {
        for (const auto* sym : f.symbols) {
            if (sym->symbol.test_scaffold) continue;
            auto t = sym->symbol.type;
            if (t != SymbolType::Function && t != SymbolType::Method &&
                t != SymbolType::Constructor)
                continue;
            nodes.push_back(sym->id);
            meta[sym->id] = {sym->symbol.name,
                             git_rel(f.path, project_root) + ":" +
                                 std::to_string(sym->symbol.line)};
            layer[sym->id] = LayerAnalyzer::classify_symbol_to_layer(*sym);
        }
    }

    analysis::CallGraph graph;
    graph.build(nodes,
                [&ref](SymbolID id) { return ref.get_callee_symbols(id); });
    auto reach = graph.incoming_reach();

    GraphSignals sig;
    auto name_at = [&](int idx) -> const std::string& {
        return meta[graph.id_at(idx)].first;
    };

    // Load-bearing. Tiny bodies (one-line trait impls, pass-through
    // accessors: Glob::as_ref, Error::clone) are excluded from the display —
    // whatever their reach, there is nothing in them to act on, and they
    // crowded out the real load-bearing functions on every Rust audit.
    absl::flat_hash_set<SymbolID> tiny_methods;
    for (const auto& f : files) {
        for (const auto* sym : f.symbols) {
            if (sym->symbol.type == SymbolType::Method &&
                sym->symbol.end_line - sym->symbol.line + 1 <= 3)
                tiny_methods.insert(sym->id);
        }
    }
    for (int i = 0; i < graph.node_count(); ++i) {
        if (reach[i] <= 0) continue;
        SymbolID id = graph.id_at(i);
        if (tiny_methods.contains(id)) continue;
        const auto& m = meta[id];
        sig.load_bearing.push_back({m.first, m.second, reach[i]});
    }
    std::sort(sig.load_bearing.begin(), sig.load_bearing.end(),
              [](const LoadBearingSym& a, const LoadBearingSym& b) {
                  if (a.reach != b.reach) return a.reach > b.reach;
                  if (a.name != b.name) return a.name < b.name;
                  return a.location < b.location;
              });
    if (sig.load_bearing.size() > top_n) sig.load_bearing.resize(top_n);

    // Brokers (betweenness). Brandes is O(V·(V+E)); skip on very large graphs so
    // the interactive overview stays fast — brokers are an optional enrichment,
    // not a correctness path.
    if (graph.node_count() <= 2000) {
        auto bc = graph.betweenness();
        std::vector<BrokerSym> brokers;
        for (int i = 0; i < graph.node_count(); ++i) {
            if (bc[i] <= 0.0) continue;
            const auto& m = meta[graph.id_at(i)];
            brokers.push_back({m.first, m.second, bc[i]});
        }
        std::sort(brokers.begin(), brokers.end(),
                  [](const BrokerSym& a, const BrokerSym& b) {
                      if (a.score != b.score) return a.score > b.score;
                      if (a.name != b.name) return a.name < b.name;
                      return a.location < b.location;
                  });
        if (brokers.size() > top_n) brokers.resize(top_n);
        // Rescale to the top broker (relative betweenness): absolute Brandes
        // scores normalized by (n-1)(n-2) round to 0.00 on any real corpus,
        // which reads as a dead metric (a zero-value leaderboard is a false
        // signal). Top row = 1.00, the rest read as fractions of it.
        if (!brokers.empty() && brokers.front().score > 0.0) {
            double top = brokers.front().score;
            for (auto& b : brokers) b.score /= top;
        }
        sig.brokers = std::move(brokers);
    }

    // Cycles (top few, each showing up to 3 members + the file it lives in).
    // Single-node SCCs are direct recursion — a property of one function,
    // not an architectural cycle — and go to their own compact list so the
    // CYCLES section only ever shows genuine multi-symbol loops.
    for (auto& cyc : graph.cycles()) {
        if (cyc.size() == 1) {
            if (sig.recursion.size() < 8) {
                const auto& m = meta[graph.id_at(cyc.front())];
                sig.recursion.emplace_back(m.first, m.second);
            }
            continue;
        }
        if (sig.cycles.size() >= 5) continue;
        CycleGroup g;
        g.total_size = static_cast<int>(cyc.size());
        for (int idx : cyc) {
            g.names.push_back(name_at(idx));
            if (g.names.size() >= 3) break;
        }
        std::sort(g.names.begin(), g.names.end());
        // File of the first member: location is "path:line".
        const std::string& loc = meta[graph.id_at(cyc.front())].second;
        auto colon = loc.rfind(':');
        g.file = colon == std::string::npos ? loc : loc.substr(0, colon);
        sig.cycles.push_back(std::move(g));
    }

    // Clusters: Louvain communities, ranked by size, with highest-reach
    // exemplars. Only meaningful when there is real structure (≥2 communities).
    auto comm = graph.louvain_communities(sig.modularity);
    if (!comm.empty()) {
        int k = 0;
        for (int c : comm) k = std::max(k, c + 1);
        sig.community_count = k;
        std::vector<std::vector<int>> members(k);
        for (int i = 0; i < static_cast<int>(comm.size()); ++i)
            members[comm[i]].push_back(i);
        std::vector<ClusterInfo> all;
        for (int c = 0; c < k; ++c) {
            if (members[c].size() < 2) continue;  // skip singletons
            auto& mem = members[c];
            std::sort(mem.begin(), mem.end(), [&](int a, int b) {
                if (reach[a] != reach[b]) return reach[a] > reach[b];
                return name_at(a) < name_at(b);
            });
            ClusterInfo ci;
            ci.size = static_cast<int>(mem.size());
            for (int idx : mem) {
                ci.exemplars.push_back(name_at(idx));
                if (ci.exemplars.size() >= 3) break;
            }

            // Label-coherent domain: the dominant propagated @lci: label across
            // this community's members. Crossing graph structure (who calls
            // whom) with propagated semantics (what concept reaches here) turns
            // an anonymous cluster into a named domain. `impure`/`pure` are
            // purity signals, not domains — excluded.
            if (propagator) {
                absl::flat_hash_map<std::string, int> label_members;
                for (int idx : mem) {
                    absl::flat_hash_set<std::string> here;
                    for (const auto& pl :
                         propagator->get_labels(graph.id_at(idx))) {
                        if (pl.strength < 0.1) continue;
                        if (pl.label == "impure" || pl.label == "pure") continue;
                        if (here.insert(pl.label).second)
                            ++label_members[pl.label];
                    }
                }
                std::string best;
                int best_n = 0;
                for (const auto& [lbl, cnt] : label_members) {
                    if (cnt > best_n || (cnt == best_n && lbl < best)) {
                        best_n = cnt;
                        best = lbl;
                    }
                }
                double coh = ci.size > 0
                                 ? static_cast<double>(best_n) / ci.size
                                 : 0.0;
                if (!best.empty() && coh >= 0.5) {
                    ci.domain = best;
                    ci.coherence = coh;
                }
            }
            all.push_back(std::move(ci));
        }
        std::sort(all.begin(), all.end(), [](const ClusterInfo& a,
                                             const ClusterInfo& b) {
            if (a.size != b.size) return a.size > b.size;
            return a.exemplars < b.exemplars;
        });
        if (all.size() > 6) all.resize(6);
        sig.clusters = std::move(all);
    }

    // Layer violations: call edges that run UP the architectural stack (a deeper
    // layer calling a shallower one). Calls should flow downward; an upward edge
    // (e.g. Data -> Presentation) inverts the dependency and is a violation.
    // Utility/unknown layers are exempt (depth -1).
    //
    // Majority-flow evidence gate (graph technique over the name-heuristic
    // layer labels): an upward edge is only a meaningful violation where the
    // codebase actually LAYERS those two labels — i.e. the downward flow
    // between the pair clearly dominates. Where flows are balanced, the
    // heuristic layer assignment itself is the suspect, and reporting the
    // edge as an architecture defect is the anti-signal the audits flagged.
    // First pass: per ordered layer pair, count edges each way.
    absl::flat_hash_map<int, std::pair<int, int>> pair_flow;  // key: hi*4+lo
    auto pair_key = [](int shallow, int deep) { return deep * 4 + shallow; };
    struct Upward {
        SymbolID u, v;
        int du, dv;
    };
    std::vector<Upward> upward;
    for (SymbolID u : nodes) {
        int du = layer_depth(layer[u]);
        if (du < 0) continue;
        for (SymbolID v : ref.get_callee_symbols(u)) {
            auto lv = layer.find(v);
            if (lv == layer.end()) continue;
            int dv = layer_depth(lv->second);
            if (dv < 0 || du == dv) continue;
            if (du < dv) {
                // Downward (correct direction) for the pair (du, dv).
                pair_flow[pair_key(du, dv)].first++;
            } else {
                pair_flow[pair_key(dv, du)].second++;
                if (!is_middleware_chain_call(meta[u].first, meta[v].first))
                    upward.push_back({u, v, du, dv});
            }
        }
    }
    for (const auto& e : upward) {
        const auto& flow = pair_flow[pair_key(e.dv, e.du)];
        // Evidence bar: at least 4 downward edges and a 3:1 dominance.
        // Below it the two labels are not demonstrably layered in this
        // codebase, so the edge is withheld rather than guessed.
        if (flow.first < 4 || flow.first < 3 * flow.second) continue;
        sig.layer_violations.push_back(
            {meta[e.u].first, layer[e.u], meta[e.v].first, layer[e.v]});
    }
    std::sort(sig.layer_violations.begin(), sig.layer_violations.end(),
              [](const LayerViolation& a, const LayerViolation& b) {
                  if (a.caller != b.caller) return a.caller < b.caller;
                  return a.callee < b.callee;
              });
    if (sig.layer_violations.size() > 8) sig.layer_violations.resize(8);

    return sig;
}

// == LOAD BEARING == — symbols the rest of the codebase most depends on, by
// transitive call-graph reach (C++ enrichment; Go has no equivalent). Pairs
// with HEALTH's problematic_symbols: high reach + high risk = fix-first.
void emit_load_bearing(std::ostringstream& out, const GraphSignals& sig) {
    const auto& lb = sig.load_bearing;
    if (lb.empty() && sig.brokers.empty()) return;
    out << "== LOAD BEARING ==\n";
    for (const auto& s : lb) {
        out << "  " << s.name << " (" << s.location << ") reach=" << s.reach
            << "\n";
    }
    // Brokers: high-betweenness chokepoints bridging separate regions. Distinct
    // from reach — a bridge can have low reach but be on every cross-path.
    if (!sig.brokers.empty()) {
        out << "brokers:\n";
        for (const auto& b : sig.brokers) {
            out << "  " << b.name << " (" << b.location
                << ") betweenness=" << fmt2(b.score) << "\n";
        }
    }
    out << "---\n";
}

// == CYCLES == — circular call dependencies (strongly-connected components of
// the call graph). C++ enrichment; Go has no equivalent.
void emit_cycles(
    std::ostringstream& out, const std::vector<CycleGroup>& cycles,
    const std::vector<std::pair<std::string, std::string>>& recursion) {
    if (cycles.empty() && recursion.empty()) return;
    out << "== CYCLES ==\n";
    out << "count=" << cycles.size() << "\n";
    for (const auto& c : cycles) {
        out << "  ";
        for (size_t i = 0; i < c.names.size(); ++i) {
            if (i) out << " -> ";
            out << c.names[i];
        }
        // Close the loop back to the first member so it reads as a cycle.
        if (!c.names.empty()) out << " -> " << c.names.front();
        out << " (" << c.file << ")";
        if (c.total_size > static_cast<int>(c.names.size()))
            out << " [+" << (c.total_size - c.names.size()) << " more]";
        out << "\n";
    }
    if (!recursion.empty()) {
        out << "recursion=";
        for (size_t i = 0; i < recursion.size(); ++i) {
            if (i) out << ", ";
            out << recursion[i].first << " (" << recursion[i].second << ")";
        }
        out << "\n";
    }
    out << "---\n";
}

// == LAYER VIOLATIONS == — calls that run UP the architectural stack (a deeper
// layer calling a shallower one), inverting the intended dependency direction.
// C++ enrichment; Go has no equivalent.
void emit_layer_violations(std::ostringstream& out,
                           const std::vector<LayerViolation>& v) {
    if (v.empty()) return;
    out << "== LAYER VIOLATIONS ==\n";
    // The layers come from name/path inference, not declared architecture —
    // say so, or a mislabeled helper reads as a confirmed violation (field
    // run: an interpreter's arg validators labeled "Presentation Layer").
    out << "layers=heuristic (name/path inference; treat as leads, not "
           "verdicts); upward edges report only where downward flow "
           "dominates the layer pair (flow evidence >=4 edges, 3:1)\n";
    out << "count=" << v.size() << "\n";
    for (const auto& x : v) {
        out << "  " << x.caller << " [" << x.caller_layer << "] -> " << x.callee
            << " [" << x.callee_layer << "]\n";
    }
    out << "---\n";
}

// == CLUSTERS == — Louvain communities over the call graph: groups of symbols
// that call each other more than the rest of the codebase, with the modularity
// score. Real graph clustering, not directory/name heuristics. C++ enrichment.
void emit_clusters(std::ostringstream& out, const GraphSignals& sig) {
    if (sig.clusters.empty()) return;
    out << "== CLUSTERS ==\n";
    out << "communities=" << sig.community_count
        << " modularity=" << fmt2(sig.modularity) << "\n";
    for (size_t i = 0; i < sig.clusters.size(); ++i) {
        const auto& c = sig.clusters[i];
        out << "  c" << i << " size=" << c.size;
        if (!c.domain.empty())
            out << " domain=" << c.domain << " coherence=" << fmt2(c.coherence);
        out << ": ";
        for (size_t j = 0; j < c.exemplars.size(); ++j) {
            if (j) out << ", ";
            out << c.exemplars[j];
        }
        out << "\n";
    }
    out << "---\n";
}

// == VOCABULARY == — low-discoverability naming signal (C++ enhancement; Go
// has no equivalent section). `outliers` are important symbols whose names use
// unknown/obscure vocabulary an agent won't search for; `aliases_in_use` tells
// which member term each standard concept uses in this codebase.
ImportDeps compute_import_dependencies(
    MasterIndex& indexer, const std::vector<FileSymbolData>& files,
    std::string_view project_root) {
    ImportDeps deps;
    // Known package dirs (normalized to '/'-separated, as emitted).
    absl::flat_hash_set<std::string> pkgs;
    absl::flat_hash_map<std::string, std::string> file_pkg;  // path -> pkg
    for (const auto& f : files) {
        auto pkg = CouplingAnalyzer::get_package_name(
            f.path, std::string(project_root));
        pkgs.insert(pkg);
        file_pkg[f.path] = std::move(pkg);
    }
    // Last-segment -> pkg, only when unique (PHP namespaces don't carry the
    // src/ prefix; a unique trailing segment is decisive, anything else is
    // dropped rather than guessed).
    absl::flat_hash_map<std::string, std::string> last_seg;
    absl::flat_hash_set<std::string> ambiguous_seg;
    for (const auto& p : pkgs) {
        auto slash = p.rfind('/');
        std::string seg = slash == std::string::npos ? p : p.substr(slash + 1);
        if (ambiguous_seg.contains(seg)) continue;
        if (auto it = last_seg.find(seg); it != last_seg.end()) {
            if (it->second != p) {
                last_seg.erase(it);
                ambiguous_seg.insert(seg);
            }
        } else {
            last_seg[seg] = p;
        }
    }
    auto resolve_import = [&](std::string imp,
                              bool seg_fallback) -> std::string {
        for (auto& c : imp)
            if (c == '\\' || c == '.') c = '/';
        while (!imp.empty() && imp.back() == '/') imp.pop_back();
        // Longest known package that is a whole-segment suffix of the import.
        std::string best;
        for (const auto& p : pkgs) {
            if (imp.size() < p.size()) continue;
            if (imp.compare(imp.size() - p.size(), p.size(), p) != 0) continue;
            if (imp.size() > p.size() && imp[imp.size() - p.size() - 1] != '/')
                continue;
            if (p.size() > best.size()) best = p;
        }
        if (!best.empty()) return best;
        // PHP/JS imports name a CLASS or file inside the package: retry with
        // trailing segments peeled (GuzzleHttp/Cookie/CookieJar ->
        // GuzzleHttp/Cookie -> matches src/Cookie by last segment). NEVER for
        // full-path import languages (Go): "encoding/json" must not land on
        // a package that happens to end in /json.
        if (!seg_fallback) return {};
        std::string probe = imp;
        for (int peel = 0; peel < 3 && !probe.empty(); ++peel) {
            auto slash = probe.rfind('/');
            std::string seg =
                slash == std::string::npos ? probe : probe.substr(slash + 1);
            if (auto it = last_seg.find(seg); it != last_seg.end())
                return it->second;
            if (slash == std::string::npos) break;
            probe.resize(slash);
        }
        return {};
    };
    // The repo's own Go module identity maps to "(root)" (chi's middleware
    // imports github.com/go-chi/chi/v5 — textually unmatchable otherwise).
    std::string go_module;
    {
        std::ifstream gm(std::filesystem::path(project_root) / "go.mod");
        std::string line;
        while (gm && std::getline(gm, line)) {
            std::istringstream ls(line);
            std::string tok;
            ls >> tok;
            if (tok == "module") {
                ls >> go_module;
                break;
            }
        }
    }
    auto resolve_with_root = [&](const std::string& imp_raw,
                                 bool seg_fallback) -> std::string {
        if (!go_module.empty()) {
            if (imp_raw == go_module) return "(root)";
            if (imp_raw.size() > go_module.size() + 1 &&
                imp_raw.rfind(go_module, 0) == 0 &&
                imp_raw[go_module.size()] == '/') {
                // module/vN → root; module/sub/pkg handled by suffix match.
                auto rest = imp_raw.substr(go_module.size() + 1);
                if (rest.size() >= 2 && rest[0] == 'v' &&
                    std::isdigit(static_cast<unsigned char>(rest[1])))
                    return "(root)";
            }
        }
        return resolve_import(imp_raw, seg_fallback);
    };
    ImportResolver ir;
    const auto& store = indexer.file_content_store();
    for (auto fid : indexer.get_all_file_ids()) {
        auto path = indexer.get_file_path(fid);
        auto it = file_pkg.find(path);
        if (it == file_pkg.end()) continue;  // outside the analysis set
        auto content = store.get_content(fid);
        if (content.empty()) continue;
        auto data = ir.extract_file_imports(fid, path, content);
        for (const auto& b : data.bindings) {
            bool seg_fallback =
                [&] {
                    auto fam = language_info_for_path(path).family;
                    return fam == LangFamily::kPhp ||
                           fam == LangFamily::kJsTs;
                }();
            auto target = resolve_with_root(b.source_file, seg_fallback);
            if (target.empty() || target == it->second) continue;
            deps.in[target].insert(it->second);
            deps.out[it->second].insert(target);
        }
    }
    return deps;
}

// == DEPENDENCIES == — which modules the rest of the codebase leans on
// (afferent coupling = number of other packages that depend on this one) and
// how unstable each is. Sourced from CouplingAnalyzer (the engine's dependency
// graph is still a node-only stub). C++-only session-startup section.
void emit_dependencies(std::ostringstream& out, const ImportDeps& deps) {
    std::vector<std::pair<std::string, int>> aff;
    aff.reserve(deps.in.size());
    for (const auto& [pkg, srcs] : deps.in)
        aff.emplace_back(pkg, static_cast<int>(srcs.size()));
    aff.erase(std::remove_if(aff.begin(), aff.end(),
                             [](const auto& p) { return p.second <= 0; }),
              aff.end());
    if (aff.empty()) return;
    std::sort(aff.begin(), aff.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    out << "== DEPENDENCIES ==\n";
    out << "most_depended_on:\n";
    size_t lim = std::min(aff.size(), size_t{8});
    for (size_t i = 0; i < lim; ++i) {
        const auto& [pkg, n] = aff[i];
        // Instability I = Ce/(Ca+Ce) over the same import-evidence counts.
        int ce = 0;
        if (auto it = deps.out.find(pkg); it != deps.out.end())
            ce = static_cast<int>(it->second.size());
        double inst = (n + ce) > 0
                          ? static_cast<double>(ce) /
                                static_cast<double>(n + ce)
                          : 0.5;
        out << "  " << pkg << " depended_on_by=" << n
            << " pkgs instability=" << fmt2(inst) << "\n";
    }
    out << "---\n";
}

// == NEXT STEPS == — tells an agent how to USE this report at session start.
// C++-only footer.

}  // namespace insight
}  // namespace mcp
}  // namespace lci
