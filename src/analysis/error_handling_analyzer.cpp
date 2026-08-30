#include <lci/analysis/error_handling_analyzer.h>

#include <lci/analysis/call_graph.h>
#include <lci/analysis/coupling_analyzer.h>
#include <lci/analysis/side_effect_analyzer.h>
#include <lci/core/reference_tracker.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <string>

namespace lci {

namespace {

std::string rel_path(std::string_view path, std::string_view root) {
    if (!root.empty() && path.rfind(root, 0) == 0) {
        path.remove_prefix(root.size());
        while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    }
    return std::string(path);
}

int severity_rank(std::string_view sev) {
    if (sev == "high") return 2;
    if (sev == "med") return 1;
    return 0;
}

void sort_findings(std::vector<EhFindingEntry>& v) {
    std::sort(v.begin(), v.end(),
              [](const EhFindingEntry& a, const EhFindingEntry& b) {
                  int ra = severity_rank(a.severity);
                  int rb = severity_rank(b.severity);
                  if (ra != rb) return ra > rb;
                  if (a.file != b.file) return a.file < b.file;
                  if (a.line != b.line) return a.line < b.line;
                  return a.signal < b.signal;
              });
}

}  // namespace

bool ErrorHandlingAnalyzer::is_swallow_signal(EhSignal s) {
    // A sentinel return is a swallow: the failure stopped here and the caller
    // was handed "no result" instead. A finally that returns is too — the
    // exception is discarded by control flow with no catch site at all.
    return s == EhSignal::EmptyCatch || s == EhSignal::CatchAndContinue ||
           s == EhSignal::LogAndSwallow || s == EhSignal::ErrorToSentinel ||
           s == EhSignal::FinallyHijacksControlFlow;
}

double ErrorHandlingAnalyzer::finding_deduction(FindingSeverity severity,
                                                double confidence,
                                                double norm_fanin) {
    double base = 0.1;
    switch (severity) {
        case FindingSeverity::High: base = 0.4; break;
        case FindingSeverity::Med: base = 0.25; break;
        case FindingSeverity::Low: base = 0.1; break;
    }
    return base * confidence * (0.5 + 0.5 * norm_fanin);
}

double ErrorHandlingAnalyzer::function_score(
    const std::vector<EhFinding>& findings, double norm_fanin,
    double contract_weight) {
    double score = 1.0;
    for (const auto& f : findings) {
        score -= contract_weight *
                 finding_deduction(f.severity, f.confidence, norm_fanin);
    }
    return std::max(0.0, score);
}

ErrorHandlingAnalyzer::Result ErrorHandlingAnalyzer::analyze(
    const SideEffectAnalyzer& analyzer, const MasterIndex& indexer,
    std::string_view project_root, const std::vector<bool>& allowed_attrs) {
    Result result;
    const auto& ref = indexer.ref_tracker();
    auto rt_snap = ref.pin();
    const auto& registry = indexer.attr_registry();
    auto file_snap = indexer.load_snapshot();

    // One production, kind-gated scoring unit per callable symbol with a
    // side-effect record.
    struct Unit {
        const SideEffectInfo* info{};
        SymbolID id{};
        std::string name;
        std::string rel;      // root-relative path
        std::string pkg;      // module (package) name
        std::string object_id;
        int line{};
        int reach{};          // filled after the graph pass
        bool exported{};      // part of the package's public surface
    };
    std::vector<Unit> units;
    std::vector<SymbolID> nodes;
    absl::flat_hash_map<SymbolID, int> unit_by_symbol;

    const auto& results = analyzer.results();
    for (FileID fid : indexer.get_all_file_ids()) {
        std::string file_path = indexer.get_file_path(fid);
        std::string rel = rel_path(file_path, project_root);
        // The file's stored attribute decides, through the same registry
        // every other tool reads. This used to be a private substring list
        // ("test", "mock", "/libs/", ...) that knew nothing about the shipped
        // ruleset or a project's `.lci.kdl`, so a benchmark harness scored as
        // product code and a configured attribute never reached this gate.
        PathAttrId attr = file_snap->attr_of(fid);
        bool production =
            allowed_attrs.empty()
                ? registry.activates(attr, Capability::Analysis)
                : (static_cast<size_t>(attr) < allowed_attrs.size() &&
                   allowed_attrs[static_cast<size_t>(attr)]);
        for (const auto& es : rt_snap->get_file_enhanced_symbols(fid)) {
            if (!es) continue;
            auto t = es->symbol.type;
            if (t != SymbolType::Function && t != SymbolType::Method &&
                t != SymbolType::Constructor)
                continue;
            nodes.push_back(es->id);
            if (!production) continue;
            std::string key =
                file_path + ":" + std::to_string(es->symbol.line) + ":0";
            auto it = results.find(key);
            if (it == results.end()) continue;
            Unit u;
            u.info = &it->second;
            u.id = es->id;
            u.name = std::string(es->symbol.name);
            u.rel = rel;
            u.pkg = CouplingAnalyzer::get_package_name(file_path, project_root);
            u.object_id = encode_symbol_id(es->id);
            u.line = es->symbol.line;
            u.exported = es->is_exported;
            unit_by_symbol[es->id] = static_cast<int>(units.size());
            units.push_back(std::move(u));
        }
    }

    // Exact transitive-caller reach over the real call graph (same signal the
    // LOAD BEARING section uses); normalized fan-in weights findings so a
    // swallow in a load-bearing function costs more than one in a leaf.
    analysis::CallGraph graph;
    graph.build(nodes,
                [&ref](SymbolID id) { return ref.get_callee_symbols(id); });
    auto reach = graph.incoming_reach();
    int max_reach = 1;
    absl::flat_hash_map<SymbolID, int> reach_by_symbol;
    reach_by_symbol.reserve(nodes.size());
    for (int i = 0; i < graph.node_count(); ++i) {
        reach_by_symbol[graph.id_at(i)] = reach[i];
        max_reach = std::max(max_reach, reach[i]);
    }
    for (auto& u : units) {
        auto it = reach_by_symbol.find(u.id);
        u.reach = it != reach_by_symbol.end() ? it->second : 0;
    }

    // --- Per-function scores + finding entries -------------------------------
    auto to_entry = [&](const Unit& u, const EhFinding& f) {
        EhFindingEntry e;
        e.severity = std::string(to_string(f.severity));
        e.signal = std::string(to_string(f.signal));
        e.symbol = u.name;
        e.file = u.rel;
        e.line = f.line;
        e.location = u.rel + ":" + std::to_string(f.line);
        e.object_id = u.object_id;
        e.detail = f.detail;
        e.confidence = f.confidence;
        return e;
    };

    absl::flat_hash_map<std::string, std::pair<double, int>> eh_by_module;
    absl::flat_hash_map<std::string, std::pair<double, int>> res_by_module;
    double eh_wsum = 0.0, eh_w = 0.0;
    double res_wsum = 0.0, res_w = 0.0;

    int throwers = 0;
    int handled = 0;
    int swallow_sites = 0;
    int unchecked_errors = 0;
    int suppressed = 0;
    int uncompensated = 0;
    int irreversible_first = 0;
    int acquisitions = 0;
    int funcs_with_acquires = 0;
    int funcs_with_release_credit = 0;
    int releases_total = 0;
    int releases_guarded = 0;
    std::vector<const Unit*> swallow_units;
    std::vector<const Unit*> funnel_units;

    for (const auto& u : units) {
        const auto& info = *u.info;
        double norm_fanin =
            static_cast<double>(u.reach) / static_cast<double>(max_reach);
        double weight = 1.0 + std::log2(1.0 + static_cast<double>(u.reach));

        // Library contract. A function on the public surface owes its
        // callers one of two things about a failure: bubble it up, or
        // transform it into this library's own error and hand THAT up. A
        // swallow inside an exported function is not a local style choice —
        // it deletes a failure the caller has no other way to learn about,
        // and no amount of caller-side diligence recovers it. Same evidence,
        // costlier verdict: the deduction is scaled, not duplicated as a
        // second finding.
        double contract_weight = u.exported ? kExportedSwallowMultiplier : 1.0;
        double eh_score =
            function_score(info.error_findings, norm_fanin, contract_weight);
        double res_score = function_score(info.resource_findings, norm_fanin,
                                          contract_weight);
        eh_wsum += weight * eh_score;
        eh_w += weight;
        res_wsum += weight * res_score;
        res_w += weight;
        eh_by_module[u.pkg].first += eh_score;
        eh_by_module[u.pkg].second += 1;
        res_by_module[u.pkg].first += res_score;
        res_by_module[u.pkg].second += 1;

        suppressed += info.suppressed_findings;
        for (const auto& f : info.error_findings) {
            result.errors.findings.push_back(to_entry(u, f));
            if (f.signal == EhSignal::DroppedError) ++unchecked_errors;
            if (f.signal == EhSignal::UncompensatedTransaction ||
                f.signal == EhSignal::PartialWriteRisk) {
                ++uncompensated;
            }
            if (f.signal == EhSignal::IrreversibleBeforeFallible) {
                ++irreversible_first;
            }
        }
        for (const auto& f : info.resource_findings) {
            result.resources.findings.push_back(to_entry(u, f));
        }

        bool swallows = false;
        bool cause_loss = false;
        for (const auto& f : info.error_findings) {
            if (is_swallow_signal(f.signal)) swallows = true;
            if (f.signal == EhSignal::RethrowNoCause) cause_loss = true;
        }
        if (swallows) {
            ++swallow_sites;
            swallow_units.push_back(&u);
        } else if (cause_loss) {
            // Generic-rethrow funnels: the error surfaces, but renamed and
            // chainless. A funnel that ALSO swallows is already seeded above.
            funnel_units.push_back(&u);
        }

        if (info.error_handling.can_throw) {
            ++throwers;
            // Handled = self or a transitive caller (bounded BFS) carries a
            // catch site.
            bool ok = info.error_handling.catch_count > 0;
            if (!ok) {
                absl::flat_hash_set<SymbolID> seen{u.id};
                std::deque<std::pair<SymbolID, int>> q{{u.id, 0}};
                while (!q.empty() && !ok) {
                    auto [sid, depth] = q.front();
                    q.pop_front();
                    if (depth >= 6) continue;
                    for (SymbolID caller : ref.get_caller_symbols(sid)) {
                        if (!seen.insert(caller).second) continue;
                        auto it = unit_by_symbol.find(caller);
                        if (it != unit_by_symbol.end() &&
                            units[it->second]
                                    .info->error_handling.catch_count > 0) {
                            ok = true;
                            break;
                        }
                        q.emplace_back(caller, depth + 1);
                    }
                }
            }
            if (ok) ++handled;
        }

        if (!info.resource_acquires.empty()) {
            ++funcs_with_acquires;
            acquisitions += static_cast<int>(info.resource_acquires.size());
            bool all_guarded_acquires = true;
            for (const auto& a : info.resource_acquires) {
                if (!a.guarded) all_guarded_acquires = false;
            }
            if (!info.resource_releases.empty() || all_guarded_acquires) {
                ++funcs_with_release_credit;
            }
        }
        for (const auto& r : info.resource_releases) {
            ++releases_total;
            if (r.guarded) ++releases_guarded;
        }
    }

    result.errors.functions_scored = static_cast<int>(units.size());
    result.resources.functions_scored = static_cast<int>(units.size());
    result.errors.throwers = throwers;
    result.errors.handled_ratio =
        throwers > 0 ? static_cast<double>(handled) / throwers : 0.0;
    result.errors.swallow_sites = swallow_sites;
    result.errors.unchecked_errors = unchecked_errors;
    result.errors.suppressed = suppressed;
    result.errors.uncompensated = uncompensated;
    result.errors.irreversible_first = irreversible_first;
    result.resources.acquisitions = acquisitions;
    result.resources.released_ratio =
        funcs_with_acquires > 0
            ? static_cast<double>(funcs_with_release_credit) /
                  funcs_with_acquires
            : 0.0;
    result.resources.guarded_ratio =
        releases_total > 0
            ? static_cast<double>(releases_guarded) / releases_total
            : 0.0;

    // --- Module + repo rollups (D3 shape: monotone, non-saturating) ----------
    auto roll = [](const absl::flat_hash_map<std::string,
                                             std::pair<double, int>>& by_mod,
                   double wsum, double w,
                   std::vector<std::pair<std::string, double>>& mod_out) {
        for (const auto& [pkg, acc] : by_mod) {
            mod_out.emplace_back(pkg, 10.0 * acc.first / acc.second);
        }
        std::sort(mod_out.begin(), mod_out.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second != b.second) return a.second < b.second;
                      return a.first < b.first;
                  });
        double repo = w > 0 ? 10.0 * wsum / w : 0.0;
        // An extreme module bounds the repo: the mean cannot average away a
        // package that swallows everywhere.
        if (!mod_out.empty()) {
            repo = std::min(repo, mod_out.front().second + 3.0);
        }
        return repo;
    };
    result.errors.score =
        roll(eh_by_module, eh_wsum, eh_w, result.errors.module_scores);
    std::vector<std::pair<std::string, double>> res_modules;  // bound only
    result.resources.score = roll(res_by_module, res_wsum, res_w, res_modules);

    sort_findings(result.errors.findings);
    sort_findings(result.resources.findings);

    // --- Exposure: public API symbols that transitively reach a sink.
    // Two seed sets, same reverse BFS:
    //  - swallow sinks (the error can vanish there), annotated with what a
    //    production log will hold when it does;
    //  - cause-loss funnels (generic rethrow: the error surfaces renamed,
    //    stack and cause chain gone) — the transformation points an
    //    incident responder needs when tracing a surfaced error backwards.
    auto reverse_bfs = [&](const std::vector<const Unit*>& seeds,
                           std::string_view kind,
                           bool annotate_log) {
        if (seeds.empty()) return;
        // Track min depth and the sink each caller first reached.
        absl::flat_hash_map<SymbolID, std::pair<int, const Unit*>> depth_sink;
        std::deque<SymbolID> q;
        for (const auto* su : seeds) {
            depth_sink[su->id] = {0, su};
            q.push_back(su->id);
        }
        while (!q.empty()) {
            SymbolID sid = q.front();
            q.pop_front();
            auto [d, sink] = depth_sink[sid];
            if (d >= 6) continue;
            for (SymbolID caller : ref.get_caller_symbols(sid)) {
                if (depth_sink.contains(caller)) continue;
                depth_sink[caller] = {d + 1, sink};
                q.push_back(caller);
            }
        }
        // What the log will hold when this sink fires: derived from the
        // sink's own findings, not from hope. empty-catch / catch-and-
        // continue / error-to-sentinel log nothing; log-and-swallow logs a
        // message or the whole error (the finding's detail already made the
        // fidelity call per language).
        auto sink_log = [](const Unit& sink) -> std::string {
            for (const auto& f : sink.info->error_findings) {
                if (f.signal != EhSignal::LogAndSwallow) continue;
                return f.detail.find("message only") != std::string::npos
                           ? "message"
                           : "full";
            }
            return "none";
        };
        std::vector<EhExposureEntry> exposure;
        for (const auto& [sid, ds] : depth_sink) {
            if (ds.first == 0) continue;  // the sink itself
            auto it = unit_by_symbol.find(sid);
            if (it == unit_by_symbol.end()) continue;
            const Unit& u = units[it->second];
            EhExposureEntry e;
            e.api_symbol = u.name;
            e.api_location = u.rel + ":" + std::to_string(u.line);
            e.sink_symbol = ds.second->name;
            e.depth = ds.first;
            e.reach = u.reach;
            e.kind = std::string(kind);
            if (annotate_log) e.log = sink_log(*ds.second);
            exposure.push_back(std::move(e));
        }
        std::sort(exposure.begin(), exposure.end(),
                  [](const EhExposureEntry& a, const EhExposureEntry& b) {
                      if (a.reach != b.reach) return a.reach > b.reach;
                      if (a.api_symbol != b.api_symbol)
                          return a.api_symbol < b.api_symbol;
                      return a.api_location < b.api_location;
                  });
        if (exposure.size() > 3) exposure.resize(3);
        for (auto& e : exposure) {
            result.errors.exposure.push_back(std::move(e));
        }
    };
    reverse_bfs(swallow_units, "swallow", /*annotate_log=*/true);
    reverse_bfs(funnel_units, "cause-loss", /*annotate_log=*/false);

    // De-saturation: 10.00 means ZERO findings, always. The weighted mean
    // rounds a large mostly-clean corpus up to 10.00 while dozens of med
    // findings stand (RxJava: 10.00 with 38), which reads as "nothing to
    // do" on a dashboard. The epsilon keeps the score honest without
    // reshaping the formula.
    if (!result.errors.findings.empty()) {
        result.errors.score = std::min(result.errors.score, 9.99);
    }
    if (!result.resources.findings.empty()) {
        result.resources.score = std::min(result.resources.score, 9.99);
    }

    // Zero-signal honesty: a perfect score computed over NOTHING is not a
    // measurement. pgvector scored 10.00 with throwers=0 because the C
    // classifier does not know Postgres's ereport/elog idiom — the corpus
    // is full of error handling the analyzer cannot see. Flag it so the
    // renderers say "no signal" instead of 10.00.
    result.errors.no_signal =
        !units.empty() && throwers == 0 && swallow_sites == 0 &&
        unchecked_errors == 0 && suppressed == 0 &&
        result.errors.findings.empty();
    result.resources.no_signal =
        !units.empty() && acquisitions == 0 &&
        result.resources.findings.empty();

    return result;
}

}  // namespace lci
