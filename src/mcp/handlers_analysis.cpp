#include <lci/mcp/handlers_analysis.h>

#include <lci/mcp/handlers_side_effects.h>
#include <lci/mcp/insight_sections.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <lci/version.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <lci/analysis/call_graph.h>
#include <lci/analysis/ci_vocabulary_analyzer.h>
#include <lci/language_map.h>
#include <lci/analysis/codebase_intelligence.h>
#include <lci/analysis/coupling_analyzer.h>
#include <lci/analysis/entry_signatures.h>
#include <lci/analysis/clone_detector.h>
#include <lci/analysis/error_handling_analyzer.h>
#include <lci/analysis/feature_analyzer.h>
#include <lci/analysis/health_analyzer.h>
#include <lci/analysis/layer_analyzer.h>
#include <lci/analysis/module_analyzer.h>
#include <lci/analysis/naming_analyzer.h>
#include <lci/analysis/side_effect_analyzer.h>
#include <lci/core/graph_propagator.h>
#include <lci/core/reference_tracker.h>
#include <lci/core/semantic_annotator.h>
#include <lci/git/analyzer.h>
#include <lci/git/frequency_analyzer.h>
#include <lci/git/provider.h>
#include <lci/git/types.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/validation.h>
#include <lci/search/search_engine.h>
#include <lci/symbol.h>

#include <absl/container/flat_hash_set.h>

namespace lci {
namespace mcp {

using namespace insight;

// -- Helpers for code_insight LCF emission -----------------------------------

namespace {

// Gather FileSymbolData from the live index, segmented by file attribute
// (PathClassifier tags stored per file at index time). code_insight's
// sections analyze the files whose attribute activates the Analysis
// capability — production by default, plus whatever a project opts in via
// `.lci.kdl`. Everything else is counted per attribute, WITH the directories
// it came from, and surfaced in == SUMMARY == so the exclusion is labeled,
// never silent. Skips files with no symbols.
struct GatheredCorpus {
    std::vector<FileSymbolData> analyzed;
    // Files WITH symbols excluded from analysis, indexed by PathAttrId.
    std::vector<ExcludedAttr> excluded;
    int excluded_total() const {
        int n = 0;
        for (const auto& e : excluded) n += e.files;
        return n;
    }
};

// First path segment of a repo-relative path — the directory a reader would
// name ("benchmarks", "tests"). Root-level files report "." so they are never
// silently dropped from the enumeration.
std::string top_dir_of(std::string_view path, std::string_view root) {
    std::string_view rel = path;
    if (!root.empty() && rel.rfind(root, 0) == 0) {
        rel.remove_prefix(root.size());
        while (!rel.empty() && rel.front() == '/') rel.remove_prefix(1);
    }
    auto slash = rel.find('/');
    if (slash == std::string_view::npos) return ".";
    return std::string(rel.substr(0, slash));
}

// Which attributes an analysis run covers. The default is the shipping
// answer: every attribute that activates Analysis. The `attributes` parameter
// overrides it — "all" for the whole corpus, or an explicit list of attribute
// names when a question really is about the tests or the benchmark harness.
// (Not "scope": git_analyze already owns that parameter name for
// staged/unstaged, and one word must mean one thing.)
struct AnalysisScope {
    std::string label;        // echoed in the LCF header
    std::vector<bool> allowed;  // indexed by PathAttrId

    bool allows(PathAttrId id) const {
        size_t i = static_cast<size_t>(id);
        return i < allowed.size() && allowed[i];
    }
};

// Parses the `attributes` parameter. Returns false and fills `error` when a named
// attribute does not exist in this project's registry — silently analyzing a
// different set than the caller asked for is worse than refusing.
bool parse_analysis_scope(const nlohmann::json& params,
                          const PathAttrRegistry& registry,
                          AnalysisScope& out, std::string& error) {
    out.allowed.assign(static_cast<size_t>(registry.size()), false);

    auto allow_named = [&](const std::string& name) {
        PathAttrId id{};
        if (!registry.find(name, id)) {
            std::string known;
            for (int i = 0; i < registry.size(); ++i) {
                if (!known.empty()) known += ", ";
                known += std::string(registry.name(static_cast<PathAttrId>(i)));
            }
            error = "unknown attribute '" + name + "' (this project has: " +
                    known + ")";
            return false;
        }
        out.allowed[static_cast<size_t>(id)] = true;
        return true;
    };

    auto it = params.find("attributes");
    if (it == params.end() || it->is_null()) {
        for (PathAttrId id : registry.with_capability(Capability::Analysis)) {
            out.allowed[static_cast<size_t>(id)] = true;
        }
        out.label = "shipping";
        return true;
    }
    if (it->is_string()) {
        std::string value = it->get<std::string>();
        if (value == "shipping") {
            for (PathAttrId id :
                 registry.with_capability(Capability::Analysis)) {
                out.allowed[static_cast<size_t>(id)] = true;
            }
            out.label = "shipping";
            return true;
        }
        if (value == "all") {
            out.allowed.assign(out.allowed.size(), true);
            out.label = "all";
            return true;
        }
        if (!allow_named(value)) return false;
        out.label = value;
        return true;
    }
    if (it->is_array()) {
        std::vector<std::string> names;
        for (const auto& entry : *it) {
            if (!entry.is_string()) {
                error = "attributes list entries must be attribute names";
                return false;
            }
            names.push_back(entry.get<std::string>());
        }
        if (names.empty()) {
            error = "attributes list is empty; omit it for shipping code";
            return false;
        }
        for (const auto& n : names) {
            if (!allow_named(n)) return false;
        }
        out.label.clear();
        for (const auto& n : names) {
            if (!out.label.empty()) out.label += ",";
            out.label += n;
        }
        return true;
    }
    error = "attributes must be \"shipping\", \"all\", an attribute name, or "
            "a list of attribute names";
    return false;
}

GatheredCorpus gather_file_symbol_data(MasterIndex& indexer,
                                       const AnalysisScope& scope) {
    GatheredCorpus corpus;
    const auto& registry = indexer.attr_registry();
    corpus.excluded.resize(static_cast<size_t>(registry.size()));
    const std::string& root = indexer.config().project.root;
    auto& ref = indexer.ref_tracker();
    auto rt_snap = ref.pin();
    auto file_snap = indexer.load_snapshot();
    for (auto fid : indexer.get_all_file_ids()) {
        auto syms = rt_snap->get_file_enhanced_symbols(fid);
        if (syms.empty()) continue;
        PathAttrId attr = file_snap->attr_of(fid);
        std::string path = indexer.get_file_path(fid);
        if (!scope.allows(attr)) {
            auto& bucket = corpus.excluded[static_cast<size_t>(attr)];
            ++bucket.files;
            ++bucket.dirs[top_dir_of(path, root)];
            continue;
        }
        FileSymbolData fsd;
        fsd.path = std::move(path);
        fsd.owner = rt_snap;
        fsd.symbols.reserve(syms.size());
        for (const auto& sym : syms) fsd.symbols.push_back(sym.get());
        corpus.analyzed.push_back(std::move(fsd));
    }
    return corpus;
}

// Render a double with two/one decimals (matches Go's "%.2f"/"%.1f" output).
// Tally a purity summary from the live SideEffectAnalyzer. Go-parity: the
// C++ SideEffectAnalyzer defaults every unanalyzed callable to pure, so
// pure-counting only happens once at least one impure function proves the
// analyzer ran for real (otherwise an unannotated corpus would report
// everything pure, diverging from Go's conservative 0/0).
// `allowed_paths`, when non-null, restricts the tally to those files — the
// analysis scope. Without it purity counted EVERY indexed file (tests,
// examples, docs_src), so its total disagreed with every other section's
// population by up to 8x (fastapi: purity total=4589 vs symbols=562).
PuritySummary tally_purity(
    SideEffectAnalyzer* analyzer,
    const absl::flat_hash_set<std::string_view>* allowed_paths = nullptr) {
    PuritySummary ps;
    if (!analyzer) return ps;
    auto in_scope = [&](const SideEffectInfo& info) {
        return allowed_paths == nullptr ||
               allowed_paths->contains(std::string_view(info.file_path));
    };
    int impure_n = 0;
    for (const auto& [key, info] : analyzer->results()) {
        (void)key;
        if (in_scope(info) && !info.is_pure) ++impure_n;
    }
    int pure_n = 0;
    if (impure_n > 0) {
        for (const auto& [key, info] : analyzer->results()) {
            (void)key;
            if (in_scope(info) && info.is_pure) ++pure_n;
        }
    }
    ps.pure_functions = pure_n;
    ps.impure_functions = impure_n;
    int total_n = 0;
    for (const auto& [key, info] : analyzer->results()) {
        (void)key;
        if (in_scope(info)) ++total_n;
    }
    ps.total_functions = total_n;
    ps.purity_ratio = ps.total_functions > 0
        ? static_cast<double>(pure_n) / ps.total_functions
        : 0.0;
    // Effect breakdown (same category bits as side_effect_summary) so the
    // HEALTH purity block can emit the `effects:` line. Counts a function once
    // per category if it (transitively) exhibits it.
    for (const auto& [key, info] : analyzer->results()) {
        (void)key;
        if (!in_scope(info)) continue;
        uint32_t combined = info.categories | info.transitive_categories;
        if (combined & side_effect::kParamWrite) ++ps.with_param_writes;
        if (combined & side_effect::kGlobalWrite) ++ps.with_global_writes;
        if (combined & (side_effect::kIO | side_effect::kNetwork |
                        side_effect::kDatabase))
            ++ps.with_io_effects;
        if (combined & side_effect::kThrow) ++ps.with_throws;
    }
    return ps;
}

// Strip a single trailing newline so the payload ends on "---" with no
// trailing newline, matching Go's strings.Join(lines, "\n").
}  // namespace

// -- handle_code_insight ------------------------------------------------------

ToolResult handle_code_insight(const nlohmann::json& raw_params,
                               CodebaseIntelligenceEngine& engine,
                               MasterIndex& indexer,
                               SideEffectAnalyzer* analyzer,
                               GraphPropagator* propagator,
                               SemanticAnnotator* sem_annotator) {
    auto params = raw_params.is_object() ? raw_params : nlohmann::json::object();
    auto mode = params.value("mode", "overview");

    if (!CodebaseIntelligenceEngine::is_valid_mode(mode)) {
        return make_error_response(
            "code_insight",
            "invalid mode '" + mode + "', must be one of: overview, detailed, "
            "statistics, unified, structure, git_analyze, git_hotspots");
    }

    AnalysisScope scope;
    std::string scope_error;
    if (!parse_analysis_scope(params, indexer.attr_registry(), scope,
                              scope_error)) {
        return make_error_response("code_insight", scope_error);
    }

    // Corpus data gathered once. file_count drives the structure summary;
    // files_data/symbol_count/project_root feed the engine-backed modes.
    const std::string& project_root = indexer.config().project.root;
    int file_count = indexer.file_count();
    auto corpus = gather_file_symbol_data(indexer, scope);
    auto& files_data = corpus.analyzed;
    int symbol_count = 0;
    for (const auto& f : files_data) {
        symbol_count += static_cast<int>(f.symbols.size());
    }
    // Full indexed path set (includes symbol-less files) — the D4 dir/file
    // census source shared by SUMMARY and the structure builder. file_attrs
    // is parallel to file_paths and carries the index-time PathClassifier
    // attribute (D1) for the structure category buckets.
    std::vector<std::string> file_paths;
    std::vector<PathAttrId> file_attrs;
    {
        auto snap = indexer.load_snapshot();
        for (auto fid : indexer.get_all_file_ids()) {
            auto p = indexer.get_file_path(fid);
            if (p.empty()) continue;
            file_paths.emplace_back(std::move(p));
            file_attrs.push_back(snap->attr_of(fid));
        }
    }

    // Shared analysis for the engine-backed modes (overview/unified/
    // statistics). The engine's overview pipeline computes the health
    // dashboard (complexity, smells, problematic symbols); modules, purity,
    // coupling/cohesion and quality are layered on from the dedicated
    // analyzers, mirroring Go's Server.buildOverview/Statistics path.
    struct EngineData {
        CodebaseIntelligenceEngine::Result result;
        ModuleAnalysis modules;        // directory map -> == REPOSITORY MAP ==
        ModuleAnalysis graph_modules;  // Louvain communities -> == MODULES ==
        PuritySummary purity;
        CouplingAnalyzer::CouplingResult coupling;
        QualityMetrics quality;
        NamingReport naming;
    };
    auto gather_engine = [&]() -> EngineData {
        CodebaseIntelligenceParams ci;
        ci.mode = "overview";
        ci.include.repository_map = true;
        ci.include.health_dashboard = true;
        ci.include.entry_points = true;
        {
            auto hints = analysis::resolve_entry_hints(
                indexer.config().insight, project_root, sem_annotator);
            ci.entry_point_pins = std::move(hints.pins);
            ci.entry_point_confidence = std::move(hints.confidence);
        }
        if (params.contains("max_results")) {
            ci.max_results = params.value("max_results", 50);
        }
        EngineData d;
        d.result = engine.analyze(ci, files_data,
                                  static_cast<int>(files_data.size()),
                                  symbol_count);
        d.modules = ModuleAnalyzer().analyze(files_data, project_root);
        {
            const auto& ref = indexer.ref_tracker();
            d.graph_modules = ModuleAnalyzer().analyze_graph(
                files_data, project_root, [&ref](SymbolID id) {
                    return ref.get_callee_symbols(id);
                });
        }
        absl::flat_hash_set<std::string_view> scope_paths;
        scope_paths.reserve(files_data.size());
        for (const auto& fd : files_data) scope_paths.insert(fd.path);
        d.purity = tally_purity(analyzer, &scope_paths);
        d.coupling = CouplingAnalyzer().analyze(
            files_data, project_root, [&indexer](SymbolID id) {
                return indexer.ref_tracker().get_outgoing_target_symbols(id);
            });
        // Replace ModuleAnalyzer's placeholder per-module coupling (a 0.30
        // constant inherited from Go) with the real per-package coupling
        // from CouplingAnalyzer. Both key modules by getPackageName, so the
        // names line up. Recompute the aggregate average from real values.
        // == MODULES == now renders graph_modules (real per-community edge
        // metrics); the directory grouping in d.modules feeds only the
        // repository map, which prints no coupling.
        if (d.result.response.health_dashboard) {
            d.quality = HealthAnalyzer::calculate_quality_from_complexity(
                d.result.response.health_dashboard->complexity);
            // Same files-based debt as build_statistics: the cc-only ratio
            // read 0.00 beside dozens of structural smells.
            d.quality.technical_debt_ratio =
                HealthAnalyzer().calculate_tech_debt_ratio_from_files(
                    files_data);
        }
        d.naming = NamingAnalyzer().analyze(
            files_data, indexer.config().synonyms, project_root,
            [&indexer](FileID fid) -> std::string_view {
                return indexer.file_content_store().get_content(fid);
            });
        return d;
    };

    std::ostringstream out;
    if (mode == "statistics") {
        // Data comes from the engine (build_statistics runs the coupling
        // analyzer + derives quality); the handler only renders LCF. purity
        // is sourced from the side-effect analyzer, which the engine does not
        // own, so it is passed in.
        if (files_data.empty()) {
            return make_error_response("code_insight",
                                       "no files provided for analysis");
        }
        CodebaseIntelligenceParams sp;
        sp.mode = "statistics";
        absl::flat_hash_set<std::string_view> scope_paths;
        scope_paths.reserve(files_data.size());
        for (const auto& fd : files_data) scope_paths.insert(fd.path);
        double purity_ratio =
            tally_purity(analyzer, &scope_paths).purity_ratio;
        auto resp = engine.build_statistics(
            sp, files_data, project_root, purity_ratio,
            [&indexer](SymbolID id) {
                return indexer.ref_tracker().get_outgoing_target_symbols(id);
            });
        const auto& sr = *resp.statistics_report;
        emit_lcf_header_scoped(out, scope.label, "statistics", 1,
                        lcf_token_count(0, 0, false, 0, true));
        emit_statistics(out, sr.complexity, sr.coupling, sr.cohesion,
                        sr.quality, sr.purity_ratio);
    } else if (mode == "structure") {
        // Structure data (dir/type/category breakdown) is computed by the
        // engine from the indexed file paths; the handler only renders LCF.
        // Counts come from the engine's D4 census (dirs = root + every
        // ancestor dir; symbols = ALL symbols with the function subset
        // labeled separately) so they agree with SUMMARY/overview. file_attrs
        // (D1 PathClassifier) drives the tests/docs/example/vendored/
        // generated category buckets.
        CodebaseIntelligenceParams sp;
        sp.mode = "structure";
        auto resp = engine.build_structure(sp, files_data, file_paths,
                                           file_attrs, indexer.attr_registry(),
                                           project_root);
        const auto& s = *resp.structure_analysis;
        out << "LCF/1.0\nmode=structure\ntier=1\ntokens=20\nattributes="
            << scope.label << "\n---\n"
            << "== STRUCTURE ==\n"
            << "dirs=" << s.dir_count << " files=" << s.file_count
            << " symbols=" << s.symbol_count
            << " functions=" << s.function_count
            << " depth=" << s.max_depth << "\n";
        out << "types:";
        for (const auto& [ext, n] : s.types) out << " " << ext << "=" << n;
        out << "\n"
            << "categories: code=" << s.code << " tests=" << s.tests
            << " config=" << s.config << " docs=" << s.docs
            << " other=" << s.other;
        // Attribute-tagged segments print only when present so corpora with
        // no tagged files keep the historical category line.
        if (s.example > 0) out << " example=" << s.example;
        if (s.vendored > 0) out << " vendored=" << s.vendored;
        if (s.generated > 0) out << " generated=" << s.generated;
        out << "\n"
            << "top_dirs:\n";
        size_t shown = std::min(s.top_dirs.size(), size_t{10});
        for (size_t i = 0; i < shown; ++i)
            out << "  " << s.top_dirs[i].first << ": " << s.top_dirs[i].second
                << " files\n";
        out << "---";
    } else if (mode == "unified") {
        EngineData d = gather_engine();
        if (!d.result.ok()) {
            return make_error_response("code_insight", d.result.error);
        }
        const auto* hd = d.result.response.health_dashboard.get();
        int n_map = std::min(static_cast<int>(d.modules.modules.size()), 15);
        bool objids = (hd && (!hd->detailed_smells.empty() ||
                              !hd->problematic_symbols.empty())) ||
                      !d.naming.outliers.empty();
        // Error-handling + resource rollups (after HEALTH, before LOAD
        // BEARING; also feeds the SUMMARY headline). Skipped when the
        // side-effect analyzer holds no records — no stubbed zeros.
        std::optional<ErrorHandlingAnalyzer::Result> eh;
        // BETA gate: the error-handling / resource report ships dark. Only
        // insight { error_report "on" } (or LCI_ERROR_REPORT=on) emits the
        // sections; "capture" generates at server shutdown without
        // publishing here.
        if (indexer.config().insight.error_report == "on" && analyzer &&
            !analyzer->results().empty()) {
            eh = ErrorHandlingAnalyzer::analyze(*analyzer, indexer,
                                                project_root, scope.allowed);
        }
        emit_lcf_header_scoped(out, scope.label, "unified", 1,
                        lcf_token_count(n_map, 0, hd != nullptr, 0, true));
        emit_summary(out, files_data, file_paths, project_root, file_count,
                     symbol_count, &corpus.excluded, &indexer.attr_registry(),
                     eh ? &*eh : nullptr);
        emit_repository_map(out, d.modules.modules);
        emit_entry_points(out, d.result.response.entry_points.get(),
                          project_root);
        if (hd) emit_health(out, *hd, &d.purity);
        if (eh) {
            emit_error_handling(out, eh->errors, 5);
            emit_resource_management(out, eh->resources, 5);
        }
        {
            auto sig = compute_graph_signals(indexer, files_data,
                                             project_root, 5, propagator);
            emit_load_bearing(out, sig);
            emit_clusters(out, sig);
            emit_cycles(out, sig.cycles, sig.recursion);
            emit_layer_violations(out, sig.layer_violations);
        }
        emit_modules(out, d.graph_modules);
        emit_dependencies(out, compute_import_dependencies(
                                   indexer, files_data, project_root));
        // == DYNAMIC == : where the code opts OUT of static analysis.
        // Dynamic dispatch (calls through an unknown-typed receiver) and
        // reflection are invisible to the call graph, so this maps how much
        // control flow is hidden, the hubs that dispatch dynamically, and
        // symbols reachable only through a dynamic call (they look dead but
        // are not). Using these features is an explicit, local decision — the
        // map tells a reader which seams need runtime reasoning/tests.
        {
            auto rt_snap = indexer.ref_tracker().pin();
            auto totals = rt_snap->call_resolution_totals();
            int all_calls =
                totals.resolved + totals.dynamic + totals.unresolved;
            if (totals.dynamic > 0) {
                double pct = all_calls > 0
                                 ? 100.0 * totals.dynamic /
                                       static_cast<double>(all_calls)
                                 : 0.0;
                out << "== DYNAMIC ==\n"
                    << "dynamic_call_sites=" << totals.dynamic << " of "
                    << all_calls << " calls (" << fmt2(pct)
                    << "% dispatch through an unknown receiver — invisible to "
                    << "the static call graph)\n";

                // Hubs: functions making the most dynamic calls (the framework
                // / plugin / registry seams).
                struct Hub {
                    std::string name;
                    std::string loc;
                    int dyn;
                };
                std::vector<Hub> hubs;
                // Reached-only-dynamically: callable, zero static callers, but
                // named by >=1 dynamic call site.
                struct DynOnly {
                    std::string name;
                    std::string loc;
                    int dyn;
                };
                std::vector<DynOnly> dyn_only;
                for (const auto& f : files_data) {
                    std::string rel = git::normalize_rel(f.path, project_root);
                    for (const auto* sym : f.symbols) {
                        if (sym == nullptr || sym->symbol.test_scaffold)
                            continue;
                        auto t = sym->symbol.type;
                        bool callable = t == SymbolType::Function ||
                                        t == SymbolType::Method ||
                                        t == SymbolType::Constructor;
                        if (!callable) continue;
                        std::string loc =
                            rel + ":" + std::to_string(sym->symbol.line);
                        int dout = rt_snap->count_dynamic_calls_out(sym->id);
                        if (dout > 0) hubs.push_back({sym->symbol.name, loc, dout});
                        if (sym->incoming_ref_count == 0 &&
                            !sym->symbol.declaration_only) {
                            auto st = rt_snap->classify_same_name_calls(
                                sym->symbol.name);
                            if (st.dynamic > 0)
                                dyn_only.push_back(
                                    {sym->symbol.name, loc, st.dynamic});
                        }
                    }
                }
                std::sort(hubs.begin(), hubs.end(),
                          [](const Hub& a, const Hub& b) {
                              if (a.dyn != b.dyn) return a.dyn > b.dyn;
                              return a.loc < b.loc;
                          });
                std::sort(dyn_only.begin(), dyn_only.end(),
                          [](const DynOnly& a, const DynOnly& b) {
                              if (a.dyn != b.dyn) return a.dyn > b.dyn;
                              return a.loc < b.loc;
                          });
                if (!hubs.empty()) {
                    out << "dynamic hubs (dispatch the most; framework/plugin "
                           "seams):\n";
                    for (size_t i = 0; i < hubs.size() && i < 5; ++i)
                        out << "  " << hubs[i].name << " (" << hubs[i].loc
                            << ") dynamic_calls=" << hubs[i].dyn << "\n";
                }
                if (!dyn_only.empty()) {
                    out << "reached only dynamically (0 static callers; look "
                           "dead but are dispatched to):\n";
                    for (size_t i = 0; i < dyn_only.size() && i < 5; ++i)
                        out << "  " << dyn_only[i].name << " ("
                            << dyn_only[i].loc
                            << ") dynamic_callers=" << dyn_only[i].dyn << "\n";
                }
                // Reflection / eval escapes: the true "can jump anywhere"
                // points, from the side-effect classifier.
                if (analyzer != nullptr) {
                    std::vector<std::string> escapes;
                    for (const auto& f : files_data) {
                        std::string rel =
                            git::normalize_rel(f.path, project_root);
                        for (const auto* sym : f.symbols) {
                            if (sym == nullptr || sym->symbol.name.empty())
                                continue;
                            auto t2 = sym->symbol.type;
                            if (t2 != SymbolType::Function &&
                                t2 != SymbolType::Method &&
                                t2 != SymbolType::Constructor)
                                continue;
                            const auto* se = analyzer->get_result(
                                f.path, sym->symbol.line);
                            if (se == nullptr) continue;
                            if (se->categories & (side_effect::kDynamicCall |
                                                  side_effect::kReflection))
                                escapes.push_back(
                                    sym->symbol.name + " (" + rel + ":" +
                                    std::to_string(sym->symbol.line) + ")");
                        }
                    }
                    std::sort(escapes.begin(), escapes.end());
                    if (!escapes.empty()) {
                        out << "reflection/eval escapes (dispatch to anything "
                               "at runtime):\n";
                        for (size_t i = 0; i < escapes.size() && i < 5; ++i)
                            out << "  " << escapes[i] << "\n";
                    }
                }
                out << "---\n";
            }
        }
        if (hd) {
            emit_statistics(out, hd->complexity, d.coupling.coupling,
                            d.coupling.cohesion, d.quality,
                            d.purity.purity_ratio);
        }
        emit_vocabulary(out, d.naming);
        if (objids) emit_object_ids_hint(out);
        emit_next_steps(out);
    } else if (mode == "git_analyze" || mode == "git_hotspots") {
        // Real git wiring. Go computes these but its LCF formatter discards the
        // git fields (emits an all-zero STATISTICS block); C++ surfaces the
        // real data — intentional enrichment, parity is envelope-only for these
        // two modes (git_hotspots is additionally time-window-volatile). A
        // non-git project root is an absent precondition, not a tool error:
        // answer with an explicit available=false block (isError would read as
        // a code failure to agent callers) — still no fake zeros.
        git::Provider provider;
        if (!git::Provider::create(project_root, provider)) {
            emit_lcf_header(out, mode, 1, lcf_token_count(0, 0, false, 0, true));
            out << "== GIT ==\n"
                << "available=false\n"
                << "reason=not a git repository: "
                << (project_root.empty() ? "<no root>" : project_root) << "\n"
                << "hint=git_analyze/git_hotspots need a git checkout; other "
                   "modes work\n";
            return ToolResult{finalize_lcf(out), false};
        }
        // Untracked root nested in an outer repo: analyzing would describe
        // THAT repo, not this project (see handle_git_analysis for the same
        // gate). Tracked monorepo subdirs pass.
        if (provider.repo_root() != project_root &&
            !provider.tracks_any(project_root)) {
            emit_lcf_header(out, mode, 1, lcf_token_count(0, 0, false, 0, true));
            out << "== GIT ==\n"
                << "available=false\n"
                << "reason=project root is untracked in the enclosing git "
                   "repository at "
                << provider.repo_root() << "\n"
                << "hint=init a repository at the project root to analyze it; "
                   "other modes work\n";
            return ToolResult{finalize_lcf(out), false};
        }

        if (mode == "git_analyze") {
            git::AnalysisParams ga = git::AnalysisParams::defaults();
            auto scope = params.value("scope", std::string("staged"));
            if (scope == "wip") ga.scope = git::AnalysisScope::WIP;
            else if (scope == "commit") ga.scope = git::AnalysisScope::Commit;
            else if (scope == "range") ga.scope = git::AnalysisScope::Range;
            else ga.scope = git::AnalysisScope::Staged;
            ga.base_ref = params.value("base_ref", std::string());
            ga.target_ref = params.value("target_ref", std::string());

            git::Analyzer analyzer(provider, indexer);
            git::AnalysisReport report;
            if (!analyzer.analyze(ga, report)) {
                // Name the inputs: the bare message on a missing ref (e.g.
                // HEAD~1 in a shallow single-commit clone) hid the cause
                // entirely.
                std::string detail = "git change analysis failed (scope=" +
                                     scope;
                if (!ga.base_ref.empty())
                    detail += " base_ref=" + ga.base_ref;
                if (!ga.target_ref.empty())
                    detail += " target_ref=" + ga.target_ref;
                detail += "); check that the refs exist in this clone "
                          "(shallow clones lack parents)";
                return make_error_response("code_insight", detail);
            }
            emit_lcf_header(out, mode, 1, lcf_token_count(0, 0, false, 0, true));
            emit_git_changes(out, report, project_root);
        } else {  // git_hotspots
            git::ChangeFrequencyParams fp = git::ChangeFrequencyParams::defaults();
            fp.time_window = params.value("time_window", std::string("30d"));
            fp.file_pattern = params.value("file_pattern", std::string());
            git::TimeWindow win = git::parse_time_window(fp.time_window);

            git::FrequencyAnalyzer freq(provider);
            git::ChangeFrequencyReport report;
            if (!freq.analyze(fp, report))
                return make_error_response("code_insight",
                                           "git frequency analysis failed");
            emit_lcf_header(out, mode, 1, lcf_token_count(0, 0, false, 0, true));
            emit_git_hotspots(out, report, win, project_root);

            // == RISK MATRIX == — churn x complexity, the "refactor here
            // first" join. Both signals existed for years (hotspots here,
            // complexity in HEALTH) but were never combined. Risk is the
            // product of the two normalized ranks; quadrant labels follow
            // the CodeScene convention.
            if (!report.hotspots.empty()) {
                absl::flat_hash_map<std::string, int> max_cc_by_rel;
                {
                    auto rt_snap = indexer.ref_tracker().pin();
                    for (auto fid : indexer.get_all_file_ids()) {
                        std::string rel = git::normalize_rel(
                            indexer.get_file_path(fid), project_root);
                        int max_cc = 0;
                        for (const auto& sym :
                             rt_snap->get_file_enhanced_symbols(fid)) {
                            if (sym && sym->complexity > max_cc)
                                max_cc = sym->complexity;
                        }
                        if (max_cc > 0) max_cc_by_rel[rel] = max_cc;
                    }
                }
                struct RiskRow {
                    std::string rel;
                    int changes;
                    int max_cc;
                    double risk;
                };
                std::vector<RiskRow> rows;
                int peak_changes = 0, peak_cc = 0;
                for (const auto& h : report.hotspots) {
                    auto it = h.metrics.find(win);
                    int changes =
                        it != h.metrics.end() ? it->second.change_count : 0;
                    if (changes <= 0) continue;
                    std::string rel =
                        git::normalize_rel(h.file_path, project_root);
                    auto cc_it = max_cc_by_rel.find(rel);
                    if (cc_it == max_cc_by_rel.end()) continue;
                    rows.push_back({std::move(rel), changes,
                                    cc_it->second, 0.0});
                    peak_changes = std::max(peak_changes, changes);
                    peak_cc = std::max(peak_cc, rows.back().max_cc);
                }
                if (!rows.empty() && peak_changes > 0 && peak_cc > 0) {
                    for (auto& r : rows) {
                        r.risk = (static_cast<double>(r.changes) /
                                  peak_changes) *
                                 (static_cast<double>(r.max_cc) / peak_cc);
                    }
                    std::sort(rows.begin(), rows.end(),
                              [](const RiskRow& a, const RiskRow& b) {
                                  if (a.risk != b.risk)
                                      return a.risk > b.risk;
                                  return a.rel < b.rel;
                              });
                    out << "== RISK MATRIX ==\n"
                        << "risk = normalized churn x normalized max "
                           "complexity over window=" << to_string(win)
                        << "\n";
                    size_t lim = std::min(rows.size(), size_t{10});
                    for (size_t i = 0; i < lim; ++i) {
                        const auto& r = rows[i];
                        bool hot = r.changes * 2 >= peak_changes;
                        bool complex_file = r.max_cc * 2 >= peak_cc;
                        const char* quadrant =
                            hot && complex_file ? "refactor-first"
                            : hot               ? "active-simple"
                            : complex_file      ? "latent-complexity"
                                                : "stable";
                        out << "  " << r.rel << " risk=" << fmt2(r.risk)
                            << " changes=" << r.changes
                            << " max_cc=" << r.max_cc << " " << quadrant
                            << "\n";
                    }
                    out << "---\n";
                }
            }
        }
    } else if (mode == "detailed") {
        // Detailed sub-mode dispatch. The engine runs the module/layer/
        // feature/vocabulary analyzers and returns the results in the
        // response; the handler renders LCF from those fields (Karpathy #6:
        // don't silently fall through to overview — surface the real data).
        std::string detailed_mode = params.value("analysis",
                                                 params.value("detailed_mode", ""));
        if (detailed_mode.empty()) detailed_mode = "modules";
        if (detailed_mode != "modules" && detailed_mode != "layers" &&
            detailed_mode != "features" && detailed_mode != "terms" &&
            detailed_mode != "errors" && detailed_mode != "resources" &&
            detailed_mode != "clones" && detailed_mode != "deadcode" &&
            detailed_mode != "security" && detailed_mode != "impact" &&
            detailed_mode != "annotate") {
            return make_error_response(
                "code_insight",
                "invalid detailed analysis '" + detailed_mode +
                "', must be one of: modules, layers, features, terms, "
                "errors, resources, clones, deadcode, security, impact, "
                "annotate");
        }

        if (detailed_mode == "clones") {
            // Corpus-wide duplicate code: exact classes over the whole
            // scoped corpus, structural classes among the largest
            // functions (bounded pairwise stage, cap reported below).
            CloneDetector::Options co;
            co.min_lines = params.value("min_lines", co.min_lines);
            co.structural_threshold =
                params.value("threshold", co.structural_threshold);
            co.max_classes = params.value("max_results", 20);
            auto rep = CloneDetector().analyze(indexer, project_root,
                                               scope.allowed, co);
            out << "LCF/1.0\nmode=detailed\nattributes=" << scope.label
                << "\nsub=clones\ntier=2\ntokens=100\n---\n";
            out << "== CLONES ==\n"
                << "functions=" << rep.functions_scanned
                << " classes=" << rep.clone_classes
                << " duplicated_lines=" << rep.duplicated_lines
                << " duplication=" << fmt2(rep.duplication_pct) << "%"
                << " (min_lines=" << co.min_lines << " structural>="
                << fmt2(co.structural_threshold) << ", structural stage "
                << "caps at the " << co.structural_top_n
                << " largest functions)\n";
            for (const auto& cc : rep.classes) {
                out << "  " << (cc.exact ? "exact" : "structural")
                    << " x" << cc.members.size() << " lines=" << cc.lines
                    << " dup=" << cc.duplicated_lines;
                if (!cc.exact) out << " sim=" << fmt2(cc.similarity);
                out << "\n";
                for (const auto& m : cc.members) {
                    out << "    " << m.name << " (" << m.path << ":"
                        << m.line << ")\n";
                }
            }
            if (rep.clone_classes >
                static_cast<int>(rep.classes.size())) {
                out << "  ... and "
                    << rep.clone_classes -
                           static_cast<int>(rep.classes.size())
                    << " smaller classes (raise max_results)\n";
            }
            std::string body = out.str();
            if (!body.empty() && body.back() == '\n') body.pop_back();
            return ToolResult{std::move(body), false};
        }

        if (detailed_mode == "impact") {
            // Blast radius of changed code (fallow parity): symbols whose
            // spans overlap the change, plus everything that TRANSITIVELY
            // calls them. scope: wip (default, uncommitted working tree),
            // staged, commit, range — same vocabulary as git_analyze.
            git::Provider provider;
            auto lcf_unavail = [&](const std::string& reason,
                                   const std::string& hint) {
                out << "LCF/1.0\nmode=detailed\nsub=impact\ntier=2"
                    << "\ntokens=30\n---\n== IMPACT ==\n"
                    << "available=false\nreason=" << reason
                    << "\nhint=" << hint << "\n";
                return ToolResult{finalize_lcf(out), false};
            };
            if (!git::Provider::create(project_root, provider)) {
                return lcf_unavail(
                    "not a git repository: " + std::string(project_root),
                    "impact reads the change set from git");
            }
            if (provider.repo_root() != project_root &&
                !provider.tracks_any(project_root)) {
                return lcf_unavail(
                    "project root is untracked in the enclosing git "
                    "repository at " + provider.repo_root(),
                    "init a repository at the project root");
            }
            git::AnalysisParams ga = git::AnalysisParams::defaults();
            auto scope_str = params.value("scope", std::string("wip"));
            if (scope_str == "wip") ga.scope = git::AnalysisScope::WIP;
            else if (scope_str == "staged")
                ga.scope = git::AnalysisScope::Staged;
            else if (scope_str == "commit")
                ga.scope = git::AnalysisScope::Commit;
            else if (scope_str == "range")
                ga.scope = git::AnalysisScope::Range;
            else
                return make_error_response(
                    "code_insight",
                    "invalid impact scope '" + scope_str +
                        "', must be wip, staged, commit, or range");
            ga.base_ref = params.value("base_ref", std::string());
            ga.target_ref = params.value("target_ref", std::string());
            ScopeSet changed;
            if (!provider.get_changed_scope(ga, changed)) {
                return lcf_unavail("git diff failed for scope=" + scope_str,
                                   "check base_ref/target_ref");
            }

            const auto& ref = indexer.ref_tracker();
            // Seed: symbols overlapping the changed line ranges.
            struct Seed {
                SymbolID id;
                std::string name;
                std::string location;
            };
            std::vector<Seed> seeds;
            absl::flat_hash_map<SymbolID,
                                std::pair<std::string, std::string>>
                sym_meta;  // id -> (name, location)
            for (const auto& f : files_data) {
                std::string rel = git::normalize_rel(f.path, project_root);
                for (const auto* sym : f.symbols) {
                    if (sym == nullptr) continue;
                    auto t = sym->symbol.type;
                    if (t != SymbolType::Function &&
                        t != SymbolType::Method &&
                        t != SymbolType::Constructor)
                        continue;
                    sym_meta[sym->id] = {
                        sym->symbol.name,
                        rel + ":" + std::to_string(sym->symbol.line)};
                    if (changed.contains_symbol(rel, *sym)) {
                        seeds.push_back(
                            {sym->id, sym->symbol.name,
                             rel + ":" +
                                 std::to_string(sym->symbol.line)});
                    }
                }
            }

            // Reverse BFS: everything that transitively calls a seed.
            absl::flat_hash_map<SymbolID, int> depth;
            std::vector<SymbolID> frontier;
            for (const auto& sd : seeds) {
                if (depth.emplace(sd.id, 0).second)
                    frontier.push_back(sd.id);
            }
            for (size_t qi = 0; qi < frontier.size(); ++qi) {
                SymbolID u = frontier[qi];
                int du = depth[u];
                for (SymbolID c : ref.get_caller_symbols(u)) {
                    if (depth.emplace(c, du + 1).second)
                        frontier.push_back(c);
                }
            }
            absl::flat_hash_set<std::string_view> entry_names;
            entry_names.insert("main");
            for (const auto& ep : indexer.config().insight.entry_points)
                entry_names.insert(ep);
            struct Affected {
                std::string name;
                std::string location;
                int depth;
            };
            std::vector<Affected> affected;
            absl::flat_hash_set<std::string> affected_files;
            int entries_hit = 0;
            for (const auto& [sid, d] : depth) {
                if (d == 0) continue;  // the seeds themselves
                auto mit = sym_meta.find(sid);
                if (mit == sym_meta.end()) continue;
                affected.push_back({mit->second.first, mit->second.second, d});
                auto colon = mit->second.second.rfind(':');
                affected_files.insert(mit->second.second.substr(0, colon));
                if (entry_names.contains(mit->second.first)) ++entries_hit;
            }
            std::sort(affected.begin(), affected.end(),
                      [](const Affected& a, const Affected& b) {
                          if (a.depth != b.depth) return a.depth < b.depth;
                          return a.location < b.location;
                      });

            int max_results = params.value("max_results", 30);
            out << "LCF/1.0\nmode=detailed\nattributes=" << scope.label
                << "\nsub=impact\ntier=2\ntokens=100\n---\n";
            out << "== IMPACT ==\n"
                << "scope=" << scope_str << " changed_symbols="
                << seeds.size() << " affected_symbols=" << affected.size()
                << " affected_files=" << affected_files.size()
                << " entry_points_affected=" << entries_hit
                << " (transitive callers of the changed symbols; static "
                   "call graph — dynamic dispatch is a lower bound; changed "
                   "symbols are limited to attributes=" + scope.label +
                   ", so test-only edits show changed_symbols=0 unless that "
                   "scope includes tests)\n";
            out << "changed:\n";
            size_t sl = std::min(seeds.size(),
                                 static_cast<size_t>(max_results));
            for (size_t i = 0; i < sl; ++i)
                out << "  " << seeds[i].name << " (" << seeds[i].location
                    << ")\n";
            if (seeds.size() > sl)
                out << "  ... and " << seeds.size() - sl << " more\n";
            if (!affected.empty()) {
                out << "affected (depth = call hops from a change):\n";
                size_t al = std::min(affected.size(),
                                     static_cast<size_t>(max_results));
                for (size_t i = 0; i < al; ++i)
                    out << "  " << affected[i].name << " ("
                        << affected[i].location
                        << ") depth=" << affected[i].depth << "\n";
                if (affected.size() > al)
                    out << "  ... and " << affected.size() - al
                        << " more\n";
            }
            std::string body = out.str();
            if (!body.empty() && body.back() == '\n') body.pop_back();
            return ToolResult{std::move(body), false};
        }

        if (detailed_mode == "security") {
            // Dangerous-sink CANDIDATES ranked by reachability from entry
            // points (fallow parity). Name evidence over the raw callee
            // spellings (sinks are usually external and unresolved), ranked
            // by forward BFS distance from mains/config entry points.
            // Candidates, not verdicts: a reachable exec with validated
            // input is fine — the ranking says where to LOOK first.
            struct Sink {
                std::string_view needle;  // matched against callee spelling
                std::string_view category;
                bool prefix;  // prefix match vs exact bare-name match
            };
            static constexpr Sink kSinks[] = {
                {"exec.Command", "process-exec", true},
                {"os.StartProcess", "process-exec", true},
                {"syscall.Exec", "process-exec", true},
                {"child_process", "process-exec", true},
                {"execSync", "process-exec", true},
                {"spawnSync", "process-exec", false},
                {"child_process.spawn", "process-exec", true},
                {"process.spawn", "process-exec", true},
                {"std.process.spawn", "process-exec", true},
                {"std.process.Child", "process-exec", true},
                {"std.process.run", "process-exec", true},
                {"os.StartProcess", "process-exec", true},
                {"popen", "process-exec", true},
                {"proc_open", "process-exec", false},
                {"shell_exec", "process-exec", false},
                {"passthru", "process-exec", false},
                {"os.system", "process-exec", true},
                {"subprocess.run", "process-exec", true},
                {"subprocess.Popen", "process-exec", true},
                {"subprocess.call", "process-exec", true},
                {"os.system", "process-exec", true},
                {"Runtime.getRuntime", "process-exec", true},
                {"ProcessBuilder", "process-exec", false},
                {"eval", "code-eval", false},
                {"exec", "code-eval", false},
                {"unserialize", "deserialization", false},
                {"pickle.loads", "deserialization", true},
                {"pickle.load", "deserialization", true},
                {"Marshal.load", "deserialization", true},
                {"yaml.load", "deserialization", true},
                {"readObject", "deserialization", false},
            };
            auto sink_of = [&](std::string_view callee)
                -> const Sink* {
                std::string_view bare = callee;
                if (auto dot = bare.rfind('.');
                    dot != std::string_view::npos)
                    bare = bare.substr(dot + 1);
                for (const auto& sk : kSinks) {
                    if (sk.prefix) {
                        if (callee.size() >= sk.needle.size() &&
                            callee.substr(0, sk.needle.size()) == sk.needle)
                            return &sk;
                    } else if (bare == sk.needle) {
                        return &sk;
                    }
                }
                return nullptr;
            };

            const auto& ref = indexer.ref_tracker();
            auto rt_snap = ref.pin();

            // Entry set + forward BFS depth over resolved call edges.
            absl::flat_hash_set<std::string_view> entry_names;
            entry_names.insert("main");
            for (const auto& ep : indexer.config().insight.entry_points)
                entry_names.insert(ep);
            absl::flat_hash_map<SymbolID, int> depth;
            std::vector<SymbolID> frontier;
            for (const auto& f : files_data) {
                for (const auto* sym : f.symbols) {
                    if (sym == nullptr) continue;
                    if (entry_names.contains(sym->symbol.name)) {
                        depth.emplace(sym->id, 0);
                        frontier.push_back(sym->id);
                    }
                }
            }
            // Zero entry points in the analysis set (a library, or every main
            // sits in excluded example/test files): reachability is UNDEFINED,
            // not "unreachable" — stamping every sink unreachable is a false
            // negative on risk (pocketbase LaunchURL IS reached from
            // apis/installer.go, but the only main is in an example file).
            const bool have_entries = !frontier.empty();
            for (size_t qi = 0; qi < frontier.size(); ++qi) {
                SymbolID u = frontier[qi];
                int du = depth[u];
                for (SymbolID v : ref.get_callee_symbols(u)) {
                    if (depth.emplace(v, du + 1).second)
                        frontier.push_back(v);
                }
            }

            struct Candidate {
                std::string category;
                std::string sink;
                std::string symbol;
                std::string location;
                int depth;  // -1 = unreachable from entry points
            };
            std::vector<Candidate> cands;
            for (const auto& f : files_data) {
                std::string rel = git::normalize_rel(f.path, project_root);
                for (const auto* sym : f.symbols) {
                    if (sym == nullptr || sym->symbol.test_scaffold) continue;
                    auto t = sym->symbol.type;
                    if (t != SymbolType::Function && t != SymbolType::Method)
                        continue;
                    for (const auto& r : rt_snap->get_symbol_references(
                             sym->id, "outgoing")) {
                        if (r.type != ReferenceType::Call) continue;
                        const Sink* sk = sink_of(r.referenced_name);
                        if (sk == nullptr) continue;
                        auto dit = depth.find(sym->id);
                        cands.push_back(
                            {std::string(sk->category), r.referenced_name,
                             sym->symbol.name,
                             rel + ":" + std::to_string(r.line),
                             dit == depth.end() ? -1 : dit->second});
                    }
                }
            }
            std::sort(cands.begin(), cands.end(),
                      [&](const Candidate& a, const Candidate& b) {
                          if (have_entries) {
                              bool ra = a.depth >= 0, rb = b.depth >= 0;
                              if (ra != rb) return ra;
                              if (a.depth != b.depth) return a.depth < b.depth;
                          }
                          if (a.location != b.location)
                              return a.location < b.location;
                          return a.sink < b.sink;
                      });

            int max_results = params.value("max_results", 30);
            out << "LCF/1.0\nmode=detailed\nattributes=" << scope.label
                << "\nsub=security\ntier=2\ntokens=100\n---\n";
            out << "== SECURITY CANDIDATES ==\n"
                << "count=" << cands.size()
                << " (name-evidence sinks ranked by call-graph distance "
                   "from entry points; candidates to review, not "
                   "verdicts)\n";
            size_t lim =
                std::min(cands.size(), static_cast<size_t>(max_results));
            for (size_t i = 0; i < lim; ++i) {
                const auto& c = cands[i];
                out << "  " << c.category << " " << c.sink << " in "
                    << c.symbol << " (" << c.location << ") ";
                if (c.depth >= 0)
                    out << "entry-reachable depth=" << c.depth;
                else if (have_entries)
                    out << "unreachable-from-entries";
                else
                    out << "reachability-unknown (no entry points in the "
                           "analysis set)";
                out << "\n";
            }
            if (cands.size() > lim)
                out << "  ... and " << cands.size() - lim
                    << " more (raise max_results)\n";
            std::string body = out.str();
            if (!body.empty() && body.back() == '\n') body.pop_back();
            return ToolResult{std::move(body), false};
        }

        if (detailed_mode == "annotate") {
            // The annotation path driver: walk an agent through every @lci:
            // annotation LCI consumes. Each dimension lists the elements that
            // would benefit, why, and the exact marker to add. `target`
            // (all | entry | domain | hotpath | deadcode) filters dimensions.
            std::string tgt = params.value("target", std::string("all"));
            if (tgt != "all" && tgt != "entry" && tgt != "domain" &&
                tgt != "hotpath" && tgt != "deadcode") {
                return make_error_response(
                    "code_insight",
                    "invalid annotate target '" + tgt +
                        "', must be all, entry, domain, hotpath, or deadcode");
            }
            bool want_entry = tgt == "all" || tgt == "entry";
            bool want_domain = tgt == "all" || tgt == "domain";
            bool want_hot = tgt == "all" || tgt == "hotpath";
            bool want_dead = tgt == "all" || tgt == "deadcode";
            int max_results = params.value("max_results", 20);

            // Annotation lookups by the annotator's synthetic (file,line,col)
            // key. Returns the annotation, or nullptr.
            auto annot_at = [&](FileID fid, int line,
                                int column) -> const SemanticAnnotation* {
                if (sem_annotator == nullptr) return nullptr;
                SymbolID key = (static_cast<SymbolID>(fid) << 32) |
                               (static_cast<SymbolID>(line) << 16) |
                               static_cast<SymbolID>(column);
                return sem_annotator->get_annotation(fid, key);
            };
            auto has_label = [](const SemanticAnnotation* a,
                                std::string_view l) {
                if (a == nullptr) return false;
                for (const auto& x : a->labels)
                    if (x == l) return true;
                return a->category == l;
            };

            auto rt_snap = indexer.ref_tracker().pin();

            out << "LCF/1.0\nmode=detailed\nattributes=" << scope.label
                << "\nsub=annotate target=" << tgt
                << "\ntier=2\ntokens=100\n---\n";
            out << "== ANNOTATION PATH ==\n"
                << "how=for each element add the shown @lci: comment above it "
                   "in source (or an .lci/annotations/*.json entry for code "
                   "you cannot edit), then re-run to see the list shrink. "
                   "Each annotation improves a specific analysis.\n";

            int pending = 0;

            if (want_entry) {
                // Exported callables ranked by transitive reach that carry no
                // entry annotation — the real front doors to confirm so
                // ENTRY POINTS stops being a heuristic guess.
                out << "-- ENTRY (confirm real front doors -> @lci:labels[entry]"
                       ", or insight { entry_points \"...\" } in .lci.kdl) --\n";
                // Entry points are call-graph ROOTS: exported callables that
                // nothing in the corpus calls (the external front door), not
                // high-reach utilities. Rank the roots by fan-OUT (how much
                // they drive) so the real front doors lead.
                struct Root { std::string name, loc; int fanout; };
                std::vector<Root> roots;
                for (const auto& f : files_data) {
                    std::string rel = git::normalize_rel(f.path, project_root);
                    for (const auto* sym : f.symbols) {
                        if (sym == nullptr) continue;
                        auto t = sym->symbol.type;
                        if (t != SymbolType::Function &&
                            t != SymbolType::Method)
                            continue;
                        if (!sym->is_exported) continue;
                        if (sym->symbol.declaration_only) continue;
                        if (sym->symbol.test_scaffold) continue;
                        if (sym->incoming_ref_count > 0) continue;  // not a root
                        if (has_label(annot_at(sym->symbol.file_id,
                                               sym->symbol.line,
                                               sym->symbol.column),
                                      "entry"))
                            continue;
                        int fanout =
                            static_cast<int>(sym->outgoing_ref_count);
                        roots.push_back(
                            {sym->symbol.name,
                             rel + ":" +
                                 std::to_string(sym->symbol.line),
                             fanout});
                    }
                }
                std::sort(roots.begin(), roots.end(),
                          [](const Root& a, const Root& b) {
                              if (a.fanout != b.fanout)
                                  return a.fanout > b.fanout;
                              return a.name < b.name;
                          });
                int shown = 0;
                for (const auto& r : roots) {
                    if (shown >= max_results) break;
                    out << "  " << r.name << " (" << r.loc
                        << ") fan_out=" << r.fanout
                        << "  reason=exported, no in-corpus caller — a "
                           "candidate front door\n";
                    ++shown;
                    ++pending;
                }
                if (shown == 0)
                    out << "  (none pending)\n";
            }

            if (want_domain) {
                // Call-graph communities with no propagated @lci: label — an
                // agent names the domain so CLUSTERS can label it.
                out << "-- DOMAIN (name the communities -> @lci:labels[<concept>]"
                       " on an exemplar; propagation spreads it) --\n";
                auto sig = compute_graph_signals(indexer, files_data,
                                                 project_root, max_results,
                                                 propagator);
                int shown = 0;
                for (const auto& c : sig.clusters) {
                    if (shown >= max_results) break;
                    if (c.size < 4) continue;          // dust
                    if (!c.domain.empty()) continue;   // already named
                    out << "  community size=" << c.size << " exemplars=";
                    for (size_t i = 0; i < c.exemplars.size() && i < 3; ++i) {
                        if (i) out << ",";
                        out << c.exemplars[i];
                    }
                    out << "  reason=no propagated domain label\n";
                    ++shown;
                    ++pending;
                }
                if (shown == 0)
                    out << "  (none pending)\n";
            }

            if (want_hot) {
                // Memory analysis hints: high-reach functions want a
                // call-frequency; recursion/cycles want a loop bound.
                out << "-- HOTPATH (memory: @lci:call-frequency[hot-path] on "
                       "high-reach; @lci:loop-bounded[N] / @lci:loop-weight[w] "
                       "on loops & recursion) --\n";
                auto sig = compute_graph_signals(indexer, files_data,
                                                 project_root, max_results,
                                                 propagator);
                int shown = 0;
                for (const auto& lb : sig.load_bearing) {
                    if (shown >= max_results) break;
                    // Skip if it already carries a frequency hint.
                    bool annotated = false;
                    for (const auto& f : files_data) {
                        for (const auto* sym : f.symbols) {
                            if (sym == nullptr || sym->symbol.name != lb.name)
                                continue;
                            const auto* a = annot_at(sym->symbol.file_id,
                                                     sym->symbol.line,
                                                     sym->symbol.column);
                            if (a && (!a->call_frequency.empty() ||
                                      a->has_memory_hints))
                                annotated = true;
                            break;
                        }
                        if (annotated) break;
                    }
                    if (annotated) continue;
                    out << "  " << lb.name << " (" << lb.location
                        << ") reach=" << lb.reach
                        << "  suggest=@lci:call-frequency[hot-path]\n";
                    ++shown;
                    ++pending;
                }
                for (const auto& r : sig.recursion) {
                    if (shown >= max_results) break;
                    out << "  " << r.first << " (" << r.second
                        << ")  reason=recursion, unbounded  "
                           "suggest=@lci:loop-bounded[N]\n";
                    ++shown;
                    ++pending;
                }
                if (shown == 0)
                    out << "  (none pending)\n";
            }

            if (want_dead) {
                // Ambiguous dead-code candidates — the deadcode flow owns the
                // full worklist; here we surface the count + pointer.
                out << "-- DEADCODE (used? -> @lci:exclude[deadcode]; dead? -> "
                       "@lci:labels[dead]; full list: analysis=deadcode "
                       "flow=true) --\n";
                int dead_candidates = 0;
                for (const auto& f : files_data) {
                    for (const auto* sym : f.symbols) {
                        if (sym == nullptr) continue;
                        auto t = sym->symbol.type;
                        bool callable = t == SymbolType::Function ||
                                        t == SymbolType::Method;
                        if (!callable) continue;
                        if (sym->symbol.declaration_only) continue;
                        if (sym->symbol.test_scaffold) continue;
                        if (sym->is_exported) continue;
                        if (sym->incoming_ref_count > 0) continue;
                        const auto& nm = sym->symbol.name;
                        if (nm == "main" || nm == "init") continue;
                        if (nm.starts_with("~") || nm.starts_with("__") ||
                            nm.starts_with("operator"))
                            continue;
                        const auto* a = annot_at(sym->symbol.file_id,
                                                 sym->symbol.line,
                                                 sym->symbol.column);
                        bool suppressed = false;
                        if (a) {
                            for (const auto& ex : a->excludes)
                                if (ex == "deadcode") suppressed = true;
                        }
                        if (suppressed) continue;
                        ++dead_candidates;
                    }
                }
                out << "  pending=" << dead_candidates << "\n";
                pending += dead_candidates;
            }

            out << "total_pending=" << pending << "\n";
            std::string body = out.str();
            if (!body.empty() && body.back() == '\n') body.pop_back();
            return ToolResult{std::move(body), false};
        }

        auto is_c_family_path = [](std::string_view rel) {
            auto dot = rel.rfind('.');
            if (dot == std::string_view::npos) return false;
            auto e = rel.substr(dot);
            return e == ".c" || e == ".h" || e == ".cpp" || e == ".hpp" ||
                   e == ".cc" || e == ".hh" || e == ".cxx" || e == ".hxx";
        };

        if (detailed_mode == "deadcode") {
            // @lci: annotations curate dead-code: static reachability cannot
            // see dynamic dispatch, function-value registration, reflection,
            // or external library consumers, so ambiguous candidates are
            // resolved by author/agent annotations. Disposition per symbol:
            //   Suppress  -> exclude[deadcode], or a used-label
            //                (api/public/entry/used/keep/dynamic/reflected/
            //                 export), or category api/endpoint/entry.
            //   Confirmed -> label 'dead' (agent has verified it; always
            //                listed, never suppressed).
            enum class Disp { None, Suppress, Confirmed };
            auto disposition = [&](FileID fid, int line, int column) -> Disp {
                if (sem_annotator == nullptr) return Disp::None;
                // SemanticAnnotator's synthetic symbol key (see
                // extract_annotations): (file<<32)|(line<<16)|column.
                SymbolID key = (static_cast<SymbolID>(fid) << 32) |
                               (static_cast<SymbolID>(line) << 16) |
                               static_cast<SymbolID>(column);
                const auto* a = sem_annotator->get_annotation(fid, key);
                if (a == nullptr) return Disp::None;
                for (const auto& l : a->labels) {
                    if (l == "dead" || l == "unused") return Disp::Confirmed;
                }
                for (const auto& ex : a->excludes)
                    if (ex == "deadcode") return Disp::Suppress;
                static constexpr std::string_view kUsed[] = {
                    "api", "public", "entry", "used", "keep",
                    "dynamic", "reflected", "export", "exported"};
                for (const auto& l : a->labels)
                    for (auto u : kUsed)
                        if (l == u) return Disp::Suppress;
                if (a->category == "api" || a->category == "endpoint" ||
                    a->category == "entry")
                    return Disp::Suppress;
                return Disp::None;
            };
            const bool flow = params.value("flow", false);
            // Flow worklist rows: everything ambiguous an agent should mark.
            struct FlowItem {
                std::string kind;    // UNUSED TYPE / DEAD EXPORT / ...
                std::string name;
                std::string location;
                std::string reason;  // why static analysis is unsure
            };
            std::vector<FlowItem> flow_items;

            // Exported symbols nothing in the corpus references. Static
            // reachability only: reflection, dynamic dispatch, and
            // external consumers of a published library are invisible, so
            // these are removal CANDIDATES, not verdicts. Methods are
            // excluded (interface/trait implementations are called
            // through their interface and would flood this with false
            // positives).
            absl::flat_hash_set<std::string_view> entry_names;
            entry_names.insert("main");
            for (const auto& ep : indexer.config().insight.entry_points) {
                entry_names.insert(ep);
            }
            struct DeadExport {
                std::string name;
                std::string path;
                int line;
                std::string type;
            };
            std::vector<DeadExport> dead;
            int exported_total = 0;
            {
                auto snap = indexer.load_snapshot();
                auto rt_snap = indexer.ref_tracker().pin();
                auto fids = indexer.get_all_file_ids();
                std::sort(fids.begin(), fids.end());
                // Constructors are invoked through their TYPE, not by a
                // name reference — a function whose name matches any
                // class/struct in the corpus is a constructor pattern and
                // would flood this list (1,327 "dead" exports on the self
                // repo before this filter, ctors/dtors everywhere).
                absl::flat_hash_set<std::string_view> type_names;
                for (auto fid : fids) {
                    for (const auto& sym :
                         rt_snap->get_file_enhanced_symbols(fid)) {
                        if (sym == nullptr) continue;
                        auto t = sym->symbol.type;
                        if (t == SymbolType::Class || t == SymbolType::Struct)
                            type_names.insert(sym->symbol.name);
                    }
                }
                for (auto fid : fids) {
                    auto attr = static_cast<size_t>(snap->attr_of(fid));
                    if (attr >= scope.allowed.size() ||
                        !scope.allowed[attr])
                        continue;
                    std::string rel = git::normalize_rel(
                        indexer.get_file_path(fid), project_root);
                    for (const auto& sym :
                         rt_snap->get_file_enhanced_symbols(fid)) {
                        if (sym == nullptr || !sym->is_exported) continue;
                        auto t = sym->symbol.type;
                        if (t != SymbolType::Function &&
                            t != SymbolType::Class &&
                            t != SymbolType::Struct)
                            continue;
                        const auto& nm = sym->symbol.name;
                        // Ctors (name == a type), dtors, and operator
                        // overloads are reached without name references; a
                        // receiver-carrying "function" is an inline class
                        // member (C++ header methods extract as Function)
                        // and gets the same method exclusion.
                        bool class_scoped = false;
                        for (const auto& sc : sym->scope_chain) {
                            if (sc.type == ScopeType::Class ||
                                sc.type == ScopeType::Struct) {
                                class_scoped = true;
                                break;
                            }
                        }
                        if (t == SymbolType::Function &&
                            (nm.starts_with("~") ||
                             nm.starts_with("operator") ||
                             type_names.contains(nm) || class_scoped))
                            continue;
                        ++exported_total;
                        Disp d = disposition(fid, sym->symbol.line,
                                              sym->symbol.column);
                        if (d == Disp::Suppress) continue;
                        bool confirmed = d == Disp::Confirmed;
                        if (!confirmed && sym->incoming_ref_count > 0) continue;
                        if (!confirmed && entry_names.contains(nm)) continue;
                        dead.push_back({sym->symbol.name, rel,
                                        static_cast<int>(sym->symbol.line),
                                        std::string(to_string(t))});
                        if (flow && !confirmed)
                            flow_items.push_back(
                                {"DEAD EXPORT", sym->symbol.name,
                                 rel + ":" +
                                     std::to_string(sym->symbol.line),
                                 "exported, zero references — may be a "
                                 "public API or dynamically used"});
                    }
                }
            }
            std::sort(dead.begin(), dead.end(),
                      [](const DeadExport& a, const DeadExport& b) {
                          if (a.path != b.path) return a.path < b.path;
                          return a.line < b.line;
                      });
            int max_results = params.value("max_results", 30);
            out << "LCF/1.0\nmode=detailed\nattributes=" << scope.label
                << "\nsub=deadcode" << (flow ? " view=annotation_flow" : "")
                << "\ntier=2\ntokens=100\n---\n";
            if (!flow)
            out << "== DEAD EXPORTS ==\n"
                << "exported=" << exported_total << " unreferenced="
                << dead.size()
                << " (static reachability only — reflection, dynamic "
                   "dispatch, and external library consumers are not "
                   "visible; methods excluded)\n";
            size_t lim = std::min(dead.size(),
                                  static_cast<size_t>(max_results));
            if (!flow)
            for (size_t i = 0; i < lim; ++i) {
                out << "  " << dead[i].type << " " << dead[i].name << " ("
                    << dead[i].path << ":" << dead[i].line << ")\n";
            }
            if (!flow && dead.size() > lim) {
                out << "  ... and " << dead.size() - lim
                    << " more (raise max_results)\n";
            }

            // fallow-depth passes over the same snapshot: unused private
            // callables, unused types, unused files. All CANDIDATES under
            // the same static-reachability caveat.
            {
                auto snap = indexer.load_snapshot();
                auto rt_snap = indexer.ref_tracker().pin();
                auto fids = indexer.get_all_file_ids();
                std::sort(fids.begin(), fids.end());

                // Names declared by any interface method spec are dispatched
                // dynamically; a private method with such a name may be an
                // implementation and must not be called dead.
                absl::flat_hash_set<std::string_view> iface_method_names;
                for (auto fid : fids) {
                    for (const auto& sym :
                         rt_snap->get_file_enhanced_symbols(fid)) {
                        if (sym != nullptr && sym->symbol.declaration_only)
                            iface_method_names.insert(sym->symbol.name);
                    }
                }

                struct DeadEntry {
                    std::string name;
                    std::string path;
                    int line;
                    std::string type;
                };
                std::vector<DeadEntry> dead_private;
                std::vector<DeadEntry> dead_types;
                struct DeadFile {
                    std::string path;
                    int symbols;
                };
                std::vector<DeadFile> dead_files;
                struct FileUnit {
                    std::string path;
                    int symbols;
                    bool external_use;
                    bool entry;
                };
                std::vector<FileUnit> file_units;

                for (auto fid : fids) {
                    auto attr = static_cast<size_t>(snap->attr_of(fid));
                    if (attr >= scope.allowed.size() || !scope.allowed[attr])
                        continue;
                    std::string rel = git::normalize_rel(
                        indexer.get_file_path(fid), project_root);
                    auto syms = rt_snap->get_file_enhanced_symbols(fid);
                    int graph_syms = 0;
                    bool any_external_use = false;
                    bool has_entry = false;
                    for (const auto& sym : syms) {
                        if (sym == nullptr) continue;
                        const auto& nm = sym->symbol.name;
                        auto t = sym->symbol.type;
                        bool callable = t == SymbolType::Function ||
                                        t == SymbolType::Method;
                        bool type_like = t == SymbolType::Class ||
                                         t == SymbolType::Struct ||
                                         t == SymbolType::Interface ||
                                         t == SymbolType::Type ||
                                         t == SymbolType::Enum;
                        if (!callable && !type_like) continue;
                        if (sym->symbol.declaration_only) continue;
                        if (sym->symbol.test_scaffold) continue;
                        ++graph_syms;
                        if (entry_names.contains(nm) || nm == "init")
                            has_entry = true;
                        // Cross-file incoming use for the file-level pass.
                        if (!any_external_use &&
                            sym->incoming_ref_count > 0) {
                            for (const auto& r : rt_snap->get_symbol_references(
                                     sym->id, "incoming")) {
                                if (r.file_id != fid) {
                                    any_external_use = true;
                                    break;
                                }
                            }
                        }
                        // A type's declaration line references its own name
                        // (type-position ref) — discount self-references
                        // before judging it unused.
                        int real_incoming = 0;
                        if (sym->incoming_ref_count > 0) {
                            if (callable) {
                                real_incoming = sym->incoming_ref_count;
                            } else {
                                for (const auto& r :
                                     rt_snap->get_symbol_references(
                                         sym->id, "incoming")) {
                                    if (r.source_symbol == sym->id) continue;
                                    if (r.file_id == fid &&
                                        r.line >= sym->symbol.line &&
                                        r.line <= sym->symbol.end_line &&
                                        r.line == sym->symbol.line)
                                        continue;
                                    ++real_incoming;
                                }
                            }
                        }
                        Disp da = disposition(fid, sym->symbol.line,
                                               sym->symbol.column);
                        if (da == Disp::Suppress) continue;
                        bool confirmed_dead = da == Disp::Confirmed;
                        if (!confirmed_dead) {
                            if (real_incoming > 0) continue;
                            if (entry_names.contains(nm) || nm == "init")
                                continue;
                            if (nm.starts_with("~") ||
                                nm.starts_with("__") ||
                                nm.starts_with("operator"))
                                continue;
                        }
                        int ln = static_cast<int>(sym->symbol.line);
                        if (callable && !sym->is_exported) {
                            if (!confirmed_dead &&
                                iface_method_names.contains(nm))
                                continue;
                            dead_private.push_back(
                                {nm, rel, ln, std::string(to_string(t))});
                            if (flow && !confirmed_dead)
                                flow_items.push_back(
                                    {"UNUSED PRIVATE", nm,
                                     rel + ":" + std::to_string(ln),
                                     "unexported, zero static references — "
                                     "may be a function value, callback, or "
                                     "dynamically dispatched"});
                        } else if (type_like) {
                            dead_types.push_back(
                                {nm, rel, ln, std::string(to_string(t))});
                            if (flow && !confirmed_dead)
                                flow_items.push_back(
                                    {"UNUSED TYPE", nm,
                                     rel + ":" + std::to_string(ln),
                                     "no indexed reference — may be used in "
                                     "a template-argument, field, receiver, "
                                     "or namespaced position not yet "
                                     "credited"});
                        }
                    }
                    // C-family files are excluded from the FILE-level pass:
                    // the declaration/implementation split (often several
                    // impl files and multi-class headers per unit) divides
                    // the use evidence, and stem pairing cannot reassemble
                    // it reliably — reference_tracker.cpp read as unused
                    // while its header is used everywhere. Symbol-level
                    // passes above still cover C++.
                    auto fam = language_info_for_path(rel).family;
                    if (graph_syms > 0 && fam != LangFamily::kCFamily) {
                        file_units.push_back(
                            {rel, graph_syms, any_external_use, has_entry});
                    }
                }
                // Header/impl pairing: a C++ impl file's use evidence lives
                // on its header's declarations (reference_tracker.cpp reads
                // as unused while reference_tracker.h is used everywhere).
                // Group by parent-dir-basename + stem and judge the UNIT.
                {
                    auto unit_key = [](const std::string& rel) {
                        std::string_view v = rel;
                        auto dot = v.rfind('.');
                        if (dot != std::string_view::npos) v = v.substr(0, dot);
                        auto s1 = v.rfind('/');
                        if (s1 == std::string_view::npos)
                            return std::string(v);
                        auto s2 = v.rfind('/', s1 - 1);
                        return std::string(
                            s2 == std::string_view::npos ? v
                                                         : v.substr(s2 + 1));
                    };
                    absl::flat_hash_map<std::string, std::pair<bool, bool>>
                        unit_state;  // key -> (any_use, any_entry)
                    for (const auto& fu : file_units) {
                        auto& st = unit_state[unit_key(fu.path)];
                        st.first |= fu.external_use;
                        st.second |= fu.entry;
                    }
                    for (const auto& fu : file_units) {
                        const auto& st = unit_state[unit_key(fu.path)];
                        if (!st.first && !st.second)
                            dead_files.push_back({fu.path, fu.symbols});
                    }
                }
                auto sort_entries = [](std::vector<DeadEntry>& v) {
                    std::sort(v.begin(), v.end(),
                              [](const DeadEntry& a, const DeadEntry& b) {
                                  if (a.path != b.path) return a.path < b.path;
                                  return a.line < b.line;
                              });
                };
                sort_entries(dead_private);
                sort_entries(dead_types);
                std::sort(dead_files.begin(), dead_files.end(),
                          [](const DeadFile& a, const DeadFile& b) {
                              if (a.symbols != b.symbols)
                                  return a.symbols > b.symbols;
                              return a.path < b.path;
                          });

                auto emit_list = [&](const char* header,
                                     const std::vector<DeadEntry>& v,
                                     const char* caveat) {
                    if (v.empty()) return;
                    out << header << "\n" << "count=" << v.size();
                    if (caveat[0] != '\0') out << " (" << caveat << ")";
                    out << "\n";
                    size_t l =
                        std::min(v.size(), static_cast<size_t>(max_results));
                    for (size_t i = 0; i < l; ++i) {
                        out << "  " << v[i].type << " " << v[i].name << " ("
                            << v[i].path << ":" << v[i].line << ")\n";
                    }
                    if (v.size() > l)
                        out << "  ... and " << v.size() - l << " more\n";
                };
                if (!flow)
                    emit_list("== UNUSED PRIVATE ==", dead_private,
                              "unexported callables with zero static "
                              "references — REVIEW candidates; function-value "
                              "and dynamic-dispatch uses are not counted, so "
                              "annotate the real ones (see annotation flow)");
                if (!flow)
                    emit_list("== UNUSED TYPES ==", dead_types,
                              "types with no indexed reference — REVIEW "
                              "candidates; template-argument/field/receiver/"
                              "namespaced uses may not be counted");
                if (flow) {
                    for (const auto& df : dead_files)
                        flow_items.push_back(
                            {"UNUSED FILE", df.path, df.path,
                             "no symbol referenced from another file and no "
                             "entry point — a language import mechanism "
                             "(@import/require/extensionless) may not be "
                             "resolved"});
                }
                if (!flow && !dead_files.empty()) {
                    out << "== UNUSED FILES ==\n"
                        << "count=" << dead_files.size()
                        << " (no symbol referenced from any other file and "
                           "no entry point; deletion candidates)\n";
                    size_t l = std::min(dead_files.size(),
                                        static_cast<size_t>(max_results));
                    for (size_t i = 0; i < l; ++i) {
                        out << "  " << dead_files[i].path << " ("
                            << dead_files[i].symbols << " syms)\n";
                    }
                    if (dead_files.size() > l)
                        out << "  ... and " << dead_files.size() - l
                            << " more\n";
                }
            }
            if (flow) {
                std::sort(flow_items.begin(), flow_items.end(),
                          [](const FlowItem& a, const FlowItem& b) {
                              if (a.kind != b.kind) return a.kind < b.kind;
                              return a.location < b.location;
                          });
                out << "== ANNOTATION FLOW ==\n"
                    << "target=deadcode pending=" << flow_items.size()
                    << "\n"
                    << "how=for each row, decide: if the element IS used "
                       "(dynamic dispatch, function value, reflection, public "
                       "API), add the @lci: marker shown above it in source "
                       "(or an .lci/annotations/*.json entry for code you "
                       "cannot edit); if it is truly dead, add "
                       "@lci:labels[dead] to confirm it. Re-run to see the "
                       "list shrink to verified-dead.\n"
                    << "markers: @lci:exclude[deadcode] (used, generic) | "
                       "@lci:labels[api] (public surface) | "
                       "@lci:labels[dynamic] (dispatched/reflected) | "
                       "@lci:labels[entry] (entry point) | "
                       "@lci:labels[dead] (confirmed dead)\n";
                size_t fl = std::min(flow_items.size(),
                                     static_cast<size_t>(max_results));
                for (size_t i = 0; i < fl; ++i) {
                    const auto& it = flow_items[i];
                    out << "  " << it.kind << " " << it.name << " ("
                        << it.location << ")\n"
                        << "    reason=" << it.reason << "\n"
                        << "    if-used: @lci:exclude[deadcode]   "
                           "if-dead: @lci:labels[dead]\n";
                }
                if (flow_items.size() > fl)
                    out << "  ... and " << flow_items.size() - fl
                        << " more (raise max_results)\n";
                if (flow_items.empty())
                    out << "(nothing pending — every candidate is either "
                           "annotated or resolved)\n";
            }
            std::string body = out.str();
            if (!body.empty() && body.back() == '\n') body.pop_back();
            return ToolResult{std::move(body), false};
        }

        if (detailed_mode == "errors" || detailed_mode == "resources") {
            // Untruncated error-handling / resource finding lists. Fail loud
            // when the side-effect analyzer holds no records (Karpathy #6) —
            // an empty section would read as "no findings".
            // Absent preconditions answer available=false, staying loud
            // (an empty section would read as "no findings") without the
            // error flag (which reads as a code failure to agent callers).
            auto lcf_unavailable = [&](const std::string& reason,
                                       const std::string& hint) {
                out << "LCF/1.0\nmode=detailed\nsub=" << detailed_mode
                    << "\ntier=2\ntokens=30\n---\n== ANALYSIS ==\n"
                    << "available=false\nreason=" << reason << "\nhint="
                    << hint << "\n";
                return ToolResult{finalize_lcf(out), false};
            };
            if (!analyzer || analyzer->results().empty()) {
                return lcf_unavailable(
                    "analysis=" + detailed_mode +
                        " requires side-effect records; the analyzer is "
                        "unpopulated for this corpus",
                    "index a corpus with functions, or check index_stats");
            }
            if (indexer.config().insight.error_report != "on") {
                return lcf_unavailable(
                    "analysis=" + detailed_mode +
                        " is BETA and disabled (insight.error_report=" +
                        indexer.config().insight.error_report + ")",
                    "enable with insight { error_report \"on\" } in "
                    ".lci.kdl or LCI_ERROR_REPORT=on");
            }
            auto eh = ErrorHandlingAnalyzer::analyze(*analyzer, indexer,
                                                     project_root,
                                                     scope.allowed);
            out << "LCF/1.0\nmode=detailed\nattributes=" << scope.label
                << "\nsub=" << detailed_mode
                << "\ntier=2\ntokens=100\n---\n";
            constexpr size_t kAll = static_cast<size_t>(-1);
            if (detailed_mode == "errors") {
                emit_error_handling(out, eh.errors, kAll);
            } else {
                emit_resource_management(out, eh.resources, kAll);
            }
            std::string body = out.str();
            if (!body.empty() && body.back() == '\n') body.pop_back();
            return ToolResult{std::move(body), false};
        }

        CodebaseIntelligenceParams dp;
        dp.mode = "detailed";
        dp.analysis = detailed_mode;
        const auto& ref = indexer.ref_tracker();
        auto resp = engine.build_detailed(
            dp, files_data, project_root,
            [&ref](SymbolID id) { return ref.get_callee_symbols(id); });

        out << "LCF/1.0\nmode=detailed\nattributes=" << scope.label
            << "\nsub=" << detailed_mode
            << "\ntier=2\ntokens=100\n---\n";
        if (detailed_mode == "modules") {
            const auto& r = *resp.module_analysis;
            const bool graph = r.detection_strategy == "call_graph_louvain";
            out << "== MODULES ==\n"
                << "total=" << r.metrics.total_modules;
            if (graph) out << " modularity=" << fmt2(r.modularity);
            out << " cohesion=" << fmt2(r.metrics.average_cohesion)
                << " coupling="
                << (r.metrics.average_coupling < 0.0
                        ? std::string("n/a")
                        : fmt2(r.metrics.average_coupling))
                << "\n"
                << "strategy=" << r.detection_strategy;
            if (graph)
                out << " (rows = declared modules/dirs; cohesion = share of "
                       "members in the dominant call-graph community)";
            out << "\n";
            size_t shown = std::min(r.modules.size(), size_t{20});
            for (size_t i = 0; i < shown; ++i) {
                const auto& m = r.modules[i];
                out << "  " << m.name << ": type=" << m.type
                    << " files=" << m.file_count
                    << " funcs=" << m.function_count
                    << " cohesion=" << fmt2(m.cohesion_score);
                if (graph) {
                    out << " clusters=" << m.community_count
                        << " coupling=" << fmt2(m.coupling_score)
                        << " deps=" << m.external_deps
                        << " instability=" << fmt2(m.stability);
                }
                out << "\n";
            }
            if (r.modules.size() > shown)
                out << "  ... and " << (r.modules.size() - shown) << " more\n";
            if (graph) {
                if (!r.split_dirs.empty()) {
                    out << "split (maps to several communities):";
                    size_t sl = std::min(r.split_dirs.size(), size_t{8});
                    for (size_t i = 0; i < sl; ++i)
                        out << " " << r.split_dirs[i];
                    if (r.split_dirs.size() > sl)
                        out << " +" << (r.split_dirs.size() - sl);
                    out << "\n";
                }
                size_t ml = std::min(r.misplaced_files.size(), size_t{12});
                if (ml > 0) {
                    out << "misplaced (joins another module's community; "
                           "shared/utility communities exempt):\n";
                    for (size_t i = 0; i < ml; ++i) {
                        const auto& f = r.misplaced_files[i];
                        out << "  " << f.file;
                        if (f.kind == "entangled") {
                            out << " entangled with " << f.belongs_with
                                << " (untangle in place)";
                        } else if (f.kind == "extension") {
                            out << " extends " << f.belongs_with
                                << " (cross-package extension point)";
                        } else {
                            out << " -> belongs with " << f.belongs_with;
                        }
                        out << " (" << f.symbols << "/" << f.total_symbols
                            << " syms)\n";
                    }
                    if (r.misplaced_files.size() > ml)
                        out << "  ... and " << (r.misplaced_files.size() - ml)
                            << " more\n";
                }
                size_t cl = std::min(r.communities.size(), size_t{12});
                if (cl > 0) {
                    out << "actual (call-graph communities; refactoring "
                           "guide):\n";
                    for (size_t i = 0; i < cl; ++i) {
                        const auto& c = r.communities[i];
                        out << "  " << c.label << ": syms=" << c.size
                            << " files=" << c.files;
                        if (c.dirs.size() > 1) {
                            out << " dirs=";
                            size_t dl = std::min(c.dirs.size(), size_t{3});
                            for (size_t d = 0; d < dl; ++d) {
                                if (d) out << ",";
                                out << c.dirs[d];
                            }
                            if (c.dirs.size() > dl)
                                out << ",+" << (c.dirs.size() - dl);
                        }
                        out << "\n";
                    }
                    if (r.communities.size() > cl)
                        out << "  ... and " << (r.communities.size() - cl)
                            << " more communities\n";
                }
                size_t tl = std::min(r.tight_couplings.size(), size_t{12});
                if (tl > 0) {
                    out << "tight coupling (calls many internals; consider "
                           "an interface):\n";
                    for (size_t i = 0; i < tl; ++i) {
                        const auto& t = r.tight_couplings[i];
                        out << "  " << t.caller << " -> " << t.callee
                            << " targets>=" << t.distinct_targets
                            << " edges=" << t.edges << "\n";
                    }
                    if (r.tight_couplings.size() > tl)
                        out << "  ... and "
                            << (r.tight_couplings.size() - tl) << " more\n";
                }
            }
        } else if (detailed_mode == "layers") {
            const auto& r = *resp.layer_analysis;
            out << "== LAYERS ==\n"
                << "total=" << r.layers.size() << "\n";
            for (const auto& l : r.layers) {
                out << "  " << l.name << ": depth=" << l.depth
                    << " modules=" << l.modules.size()
                    << " symbols=" << l.metrics.symbol_count
                    << " cohesion=" << fmt2(l.metrics.cohesion_score) << "\n";
            }
            for (const auto& p : r.patterns) {
                out << "  pattern: " << p.name
                    << " confidence=" << fmt2(p.confidence) << "\n";
            }
            out << "  next: call-edge layer violations are in "
                   "code_insight {\"mode\":\"overview\"} == LAYER "
                   "VIOLATIONS ==\n";
        } else if (detailed_mode == "features") {
            const auto& r = *resp.feature_analysis;
            out << "== FEATURES ==\n"
                << "total=" << r.metrics.total_features
                << " avg_components=" << fmt2(r.metrics.average_components)
                << " avg_cohesion=" << fmt2(r.metrics.avg_cohesion)
                << " avg_complexity=" << fmt2(r.metrics.avg_complexity) << "\n";
            size_t shown = std::min(r.features.size(), size_t{20});
            for (size_t i = 0; i < shown; ++i) {
                const auto& f = r.features[i];
                out << "  " << f.name
                    << ": module=" << f.primary_module
                    << " components=" << f.components.size()
                    << " confidence=" << fmt2(f.confidence) << "\n";
            }
            if (r.features.size() > shown)
                out << "  ... and " << (r.features.size() - shown) << " more\n";
            for (const auto& d : r.cross_feature_deps) {
                out << "  dep: " << d.feature_a << "->" << d.feature_b
                    << " type=" << d.type
                    << " strength=" << fmt2(d.strength) << "\n";
            }
        } else {  // terms
            const auto& terms = resp.domain_terms;
            out << "== TERMS ==\n"
                << "total=" << terms.size() << "\n";
            size_t shown = std::min(terms.size(), size_t{30});
            for (size_t i = 0; i < shown; ++i) {
                const auto& t = terms[i];
                out << "  " << t.domain
                    << ": count=" << t.count
                    << " confidence=" << fmt2(t.confidence)
                    << " terms=" << t.terms.size() << "\n";
            }
            if (terms.size() > shown)
                out << "  ... and " << (terms.size() - shown) << " more\n";
        }
        out << "---";
    } else {
        // overview (default) — engine-backed: repository map + health, with
        // the object-IDs workflow hint when smells/problematic symbols are
        // present. Mirrors Go's overview section set (no MODULES/STATISTICS).
        EngineData d = gather_engine();
        if (!d.result.ok()) {
            return make_error_response("code_insight", d.result.error);
        }
        const auto* hd = d.result.response.health_dashboard.get();
        int n_map = std::min(static_cast<int>(d.modules.modules.size()), 15);
        bool objids = (hd && (!hd->detailed_smells.empty() ||
                              !hd->problematic_symbols.empty())) ||
                      !d.naming.outliers.empty();
        std::optional<ErrorHandlingAnalyzer::Result> eh;
        // BETA gate: the error-handling / resource report ships dark. Only
        // insight { error_report "on" } (or LCI_ERROR_REPORT=on) emits the
        // sections; "capture" generates at server shutdown without
        // publishing here.
        if (indexer.config().insight.error_report == "on" && analyzer &&
            !analyzer->results().empty()) {
            eh = ErrorHandlingAnalyzer::analyze(*analyzer, indexer,
                                                project_root, scope.allowed);
        }
        emit_lcf_header_scoped(out, scope.label, "overview", 1,
                        lcf_token_count(n_map, 0, hd != nullptr, 0, false));
        emit_summary(out, files_data, file_paths, project_root, file_count,
                     symbol_count, &corpus.excluded, &indexer.attr_registry(),
                     eh ? &*eh : nullptr);
        emit_repository_map(out, d.modules.modules);
        emit_entry_points(out, d.result.response.entry_points.get(),
                          project_root);
        if (hd) emit_health(out, *hd, &d.purity);
        if (eh) {
            emit_error_handling(out, eh->errors, 5);
            emit_resource_management(out, eh->resources, 5);
        }
        {
            auto sig = compute_graph_signals(indexer, files_data,
                                             project_root, 5, propagator);
            emit_load_bearing(out, sig);
            emit_clusters(out, sig);
            emit_cycles(out, sig.cycles, sig.recursion);
            emit_layer_violations(out, sig.layer_violations);
        }
        emit_vocabulary(out, d.naming);
        if (objids) emit_object_ids_hint(out);
        emit_next_steps(out);
    }
    return ToolResult{finalize_lcf(out), false};
}

// -- register_analysis_handlers -----------------------------------------------

void register_analysis_handlers(McpServer& server,
                                MasterIndex* indexer,
                                SemanticAnnotator* annotator,
                                SideEffectAnalyzer* analyzer,
                                GraphPropagator* propagator,
                                CodebaseIntelligenceEngine* ci_engine) {
    // Replace "semantic_annotations" stub
    server.add_tool(
        {"semantic_annotations",
         "🏷️  Query symbols by semantic labels or categories. Supports "
         "direct @lci: annotations, external annotation manifests, and "
         "propagated labels through call graphs. See 'info "
         "semantic_annotations'.",
         {{"label", "string", "Semantic label to search for", ""},
          {"category", "string", "Semantic category", ""},
          {"min_strength", "number", "Minimum label strength", ""},
          {"include_direct", "boolean",
           "Include direct annotations", ""},
          {"include_propagated", "boolean",
           "Include propagated labels", ""},
          {"max_results", "integer",
           "Maximum results (keep small to avoid token overload)", ""}},
         {}},
        [annotator, propagator, indexer](const nlohmann::json& p) -> ToolResult {
            if (!annotator) {
                return make_unavailable_response(
                    "semantic_annotations",
                    "semantic annotator not available", "retry shortly; the server is still starting, or this build/config lacks the analyzer");
            }
            return handle_semantic_annotations(p, *annotator, propagator,
                                                indexer);
        });

    // Replace "side_effects" stub
    server.add_tool(
        {"side_effects",
         "🔬 Query function purity and side effects. Detects writes to "
         "parameters, globals, closures, I/O operations, and exception "
         "handling. Supports transitive analysis through call graphs. "
         "See 'info side_effects'.",
         {{"mode", "string",
           "Query mode: symbol, file, pure, impure, category, summary", ""},
          {"symbol_id", "string", "Symbol ID for symbol mode", ""},
          {"symbol_name", "string", "Symbol name for symbol mode", ""},
          {"file_path", "string", "File path for file mode", ""},
          {"file_id", "integer", "File ID for file mode", ""},
          {"category", "string",
           "Side effect category: param_write, global_write, io, network, "
           "throw, channel, external_call",
           ""},
          {"include_reasons", "boolean",
           "Include reasons for impurity", ""},
          {"include_transitive", "boolean",
           "Include transitive side effects from callees", ""},
          {"include_confidence", "boolean",
           "Include confidence levels", ""},
          {"max_results", "integer",
           "Maximum results (keep small to avoid token overload)", ""}},
         {}},
        [analyzer, indexer](const nlohmann::json& p) -> ToolResult {
            if (!analyzer) {
                return make_unavailable_response(
                    "side_effects",
                    "side effect analysis not available", "retry shortly; the server is still starting, or this build/config lacks the analyzer");
            }
            return handle_side_effects(p, *analyzer, indexer);
        });

    // Replace "code_insight" stub
    server.add_tool(
        {"code_insight",
         "🎯 Comprehensive codebase intelligence system for AI agents. "
         "Provides high-level overview (79.8% context reduction), detailed "
         "analysis (2-4x accuracy improvement), code statistics, and git "
         "analysis. Modes: overview, detailed, statistics, unified, "
         "structure, git_analyze, git_hotspots. See 'info code_insight'.",
         {{"mode", "string", "Analysis mode", ""},
          {"attributes", "string",
           "Which files to analyze, by file attribute: \"shipping\" (default "
           "— every attribute that activates analysis), \"all\", one "
           "attribute name, or a list (e.g. [\"test\",\"benchmark\"])",
           ""},
          {"tier", "integer", "Analysis tier", ""},
          {"analysis", "string",
           "Detailed analysis: modules, layers, features, terms, errors, "
           "resources, clones (corpus-wide duplicate code), deadcode "
           "(unused code; add flow=true for the @lci: annotation worklist), "
           "security (dangerous-sink candidates by entry reach), impact "
           "(blast radius of a git change set), annotate (the @lci: "
           "annotation path driver: what to mark and with which marker)",
           ""},
          {"metrics", "array", "Metrics to include", "string"},
          {"min_lines", "integer",
           "analysis=clones: minimum normalized body lines for a function "
           "to count (default 6)",
           ""},
          {"threshold", "number",
           "analysis=clones: structural similarity threshold 0-1 "
           "(default 0.9)",
           ""},
          {"target", "string", "analysis=annotate: which annotation dimension (all, entry, domain, hotpath, deadcode)", ""},
          {"focus", "string", "Analysis focus", ""},
          {"flow", "boolean",
           "analysis=deadcode: emit the @lci: annotation worklist (elements "
           "needing a used/dead decision) instead of the candidate lists",
           ""},
          {"max_results", "integer",
           "Maximum results (keep small to avoid token overload)", ""},
          {"scope", "string",
           "git_analyze: staged (default), wip, commit, range", ""},
          {"base_ref", "string",
           "git_analyze: base ref for commit/range (e.g. HEAD~1, main)", ""},
          {"target_ref", "string",
           "git_analyze: target ref for range (defaults to HEAD)", ""},
          {"time_window", "string",
           "git_hotspots: 7d, 30d (default), 90d, or 1y", ""},
          {"file_pattern", "string", "git_hotspots: glob filter", ""},
          {"languages", "array",
           "Filter by programming languages (e.g., [\"go\"], "
           "[\"typescript\", \"javascript\"], [\"csharp\"]). "
           "Case-insensitive with aliases (e.g., 'ts' for TypeScript, "
           "'cs' for C#).",
           "string"}},
         {},
         {"detailed_mode"}},
        [ci_engine, indexer, analyzer, propagator, annotator](
            const nlohmann::json& p) -> ToolResult {
            if (!ci_engine || !indexer) {
                return make_unavailable_response(
                    "code_insight",
                    "codebase intelligence not available", "retry shortly; the server is still starting, or this build/config lacks the analyzer");
            }
            return handle_code_insight(p, *ci_engine, *indexer, analyzer,
                                       propagator, annotator);
        });
}

std::string write_error_report_capture(MasterIndex& indexer,
                                       SideEffectAnalyzer* analyzer) {
    if (indexer.config().insight.error_report != "capture") return {};

    namespace fs = std::filesystem;
    // State dir: $XDG_STATE_HOME/lci/error-reports, ~/.local/state fallback.
    fs::path base;
    if (const char* xdg = std::getenv("XDG_STATE_HOME");
        xdg != nullptr && *xdg != '\0') {
        base = fs::path(xdg);
    } else if (const char* home = std::getenv("HOME");
               home != nullptr && *home != '\0') {
        base = fs::path(home) / ".local" / "state";
    } else {
        std::cerr << "error-report capture: no XDG_STATE_HOME or HOME\n";
        return {};
    }
    fs::path dir = base / "lci" / "error-reports";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        std::cerr << "error-report capture: cannot create " << dir.string()
                  << ": " << ec.message() << "\n";
        return {};
    }

    const std::string& root = indexer.config().project.root;
    std::string slug;
    slug.reserve(root.size());
    for (char c : root) {
        slug += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
                 c == '-')
                    ? c
                    : '-';
    }
    while (!slug.empty() && slug.front() == '-') slug.erase(slug.begin());

    std::ostringstream out;
    out << "LCI ERROR REPORT (beta capture)\n"
        << "root=" << root << "\n"
        << "generated_by=lci " << kVersion << "\n"
        << "mode=capture (insight.error_report) — not published in any tool "
           "response\n---\n";
    if (analyzer == nullptr || analyzer->results().empty()) {
        // Explicit, never silent: an empty capture file that LOOKS like a
        // clean report would be a lie. Name the reason.
        out << "no side-effect records: the AST warmup did not populate the "
               "analyzer (empty, unreadable, or unsupported corpus)\n";
    } else {
        auto eh = ErrorHandlingAnalyzer::analyze(
            *analyzer, indexer, root);
        constexpr size_t kAll = static_cast<size_t>(-1);
        emit_error_handling(out, eh.errors, kAll);
        emit_resource_management(out, eh.resources, kAll);
    }

    fs::path final_path = dir / (slug + ".txt");
    fs::path tmp_path = dir / (slug + ".txt.tmp");
    {
        std::ofstream f(tmp_path, std::ios::trunc);
        if (!f) {
            std::cerr << "error-report capture: cannot write "
                      << tmp_path.string() << "\n";
            return {};
        }
        f << out.str();
    }
    fs::rename(tmp_path, final_path, ec);
    if (ec) {
        std::cerr << "error-report capture: rename failed: " << ec.message()
                  << "\n";
        return {};
    }
    return final_path.string();
}

}  // namespace mcp
}  // namespace lci
