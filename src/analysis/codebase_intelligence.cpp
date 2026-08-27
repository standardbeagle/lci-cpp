#include <lci/analysis/codebase_intelligence.h>

#include <lci/analysis/ci_vocabulary_analyzer.h>
#include <lci/analysis/coupling_analyzer.h>
#include <lci/analysis/feature_analyzer.h>
#include <lci/analysis/health_analyzer.h>
#include <lci/analysis/layer_analyzer.h>
#include <lci/analysis/module_analyzer.h>
#include <lci/analysis/token_budget.h>
#include <lci/reference.h>
#include <lci/search/search_options.h>  // classify_file / FileCategory

#include <absl/container/flat_hash_set.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <string>

namespace lci {

namespace {

bool contains_lower(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool is_function_like(SymbolType t) {
    return t == SymbolType::Function || t == SymbolType::Method;
}

int count_functions(const std::vector<FileSymbolData>& files) {
    int count = 0;
    for (const auto& f : files) {
        for (const auto* sym : f.symbols) {
            if (is_function_like(sym->symbol.type)) ++count;
        }
    }
    return count;
}

int count_all_symbols(const std::vector<FileSymbolData>& files) {
    int count = 0;
    for (const auto& f : files) {
        count += static_cast<int>(f.symbols.size());
    }
    return count;
}

}  // namespace

// ============================================================================
// Mode validation
// ============================================================================

bool CodebaseIntelligenceEngine::is_valid_mode(std::string_view mode) {
    return mode == "overview" || mode == "detailed" || mode == "statistics" ||
           mode == "unified" || mode == "structure" || mode == "git_analyze" ||
           mode == "git_hotspots";
}

// ============================================================================
// Defaults
// ============================================================================

CodebaseIntelligenceParams CodebaseIntelligenceEngine::apply_defaults(
    CodebaseIntelligenceParams params) {
    if (params.mode.empty()) params.mode = "overview";
    if (!params.tier) params.tier = ci_defaults::kDefaultTier;
    if (*params.tier < 1 || *params.tier > 3) params.tier = ci_defaults::kDefaultTier;
    if (!params.granularity) params.granularity = ci_defaults::kDefaultGranularity;
    if (!params.confidence_threshold)
        params.confidence_threshold = ci_defaults::kDefaultConfidenceThreshold;

    if (params.mode == "overview") {
        bool any_set = params.include.repository_map ||
                       params.include.health_dashboard ||
                       params.include.entry_points;
        if (!any_set) {
            params.include.repository_map = true;
            params.include.health_dashboard = true;
            params.include.entry_points = true;
        }
    }
    return params;
}

// ============================================================================
// Importance score
// ============================================================================

double CodebaseIntelligenceEngine::calculate_importance_score(
    const EnhancedSymbol& sym) {
    double score = static_cast<double>(sym.incoming_ref_count);

    if (sym.is_exported) score *= 1.5;

    if (sym.symbol.name == "main" || sym.symbol.name == "Main") score *= 2.0;

    if (contains_lower(sym.symbol.name, "handler") ||
        contains_lower(sym.symbol.name, "controller") ||
        contains_lower(sym.symbol.name, "service")) {
        score *= 1.3;
    }

    if (sym.complexity > 0) {
        score *= (1.0 + static_cast<double>(sym.complexity) / 20.0);
    }
    return score;
}

// ============================================================================
// Main analysis pipeline
// ============================================================================

CodebaseIntelligenceEngine::Result CodebaseIntelligenceEngine::analyze(
    const CodebaseIntelligenceParams& raw_params,
    const std::vector<FileSymbolData>& files,
    int file_count, int symbol_count) const {
    auto params = apply_defaults(raw_params);

    if (!is_valid_mode(params.mode)) {
        return Result{{}, "invalid mode '" + params.mode +
                              "', must be one of: overview, detailed, "
                              "statistics, unified, structure, "
                              "git_analyze, git_hotspots"};
    }

    if (files.empty()) {
        return Result{{}, "no files provided for analysis"};
    }

    // detailed/statistics/structure need index-derived inputs (call-graph
    // edges, project root, the full file-path set) that this pre-collected-data
    // entry point cannot supply. Building them here would silently degrade the
    // output — skipped feature clustering, an empty directory tree — so fail
    // fast and direct callers to the index-backed builders, which the MCP
    // handler invokes with those inputs. (No FALLBACKS / no silent empty
    // sections.)
    if (params.mode == "detailed" || params.mode == "statistics" ||
        params.mode == "structure") {
        return Result{
            {}, "mode '" + params.mode +
                    "' is not available through the index-less analyze() path; "
                    "it requires the index-backed builders "
                    "(build_detailed/build_statistics/build_structure) with "
                    "call-graph, project-root and file-path inputs"};
    }

    auto start = std::chrono::steady_clock::now();

    CodebaseIntelligenceResponse response;
    if (params.mode == "overview") {
        response = build_overview(params, files, file_count, symbol_count);
    } else if (params.mode == "unified") {
        response = build_unified(params, files, file_count, symbol_count);
    }
    // git_analyze / git_hotspots are handled entirely in the MCP layer
    // (handle_code_insight), which owns the git::Provider; the engine has no
    // provider so it never built them. is_valid_mode still accepts them.

    auto end = std::chrono::steady_clock::now();
    int elapsed_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count());

    response.analysis_mode = params.mode;
    response.tier = params.tier.value_or(ci_defaults::kDefaultTier);
    response.analysis_metadata.analysis_time_ms = elapsed_ms;
    response.analysis_metadata.files_analyzed =
        file_count > 0 ? file_count : static_cast<int>(files.size());
    response.analysis_metadata.analyzed_at =
        std::chrono::system_clock::now();
    response.analysis_metadata.index_version = "1.0";

    TokenBudgetManager::enforce_budget(
        response,
        params.max_results ? &*params.max_results : nullptr);

    return Result{std::move(response), {}};
}

// ============================================================================
// Overview (Tier 1)
// ============================================================================

CodebaseIntelligenceResponse CodebaseIntelligenceEngine::build_overview(
    const CodebaseIntelligenceParams& params,
    const std::vector<FileSymbolData>& files,
    int file_count, int symbol_count) const {
    CodebaseIntelligenceResponse response;
    response.navigation_hints["clickable_ids"] =
        "Every entity_id is clickable - use with get_object_context";
    response.navigation_hints["navigation_flow"] =
        "Click entry_point -> see call hierarchy -> follow references";

    int max_results = params.max_results.value_or(ci_defaults::kDefaultMaxResults);

    if (params.include.repository_map) {
        auto map = std::make_unique<RepositoryMap>();
        map->total_files = file_count > 0 ? file_count
                                          : static_cast<int>(files.size());
        map->total_symbols = symbol_count > 0 ? symbol_count
                                              : count_all_symbols(files);
        map->total_functions = count_functions(files);
        map->analyzed_at = std::chrono::system_clock::now();

        map->critical_functions = extract_critical_functions(files, max_results);

        CIVocabularyAnalyzer vocab;
        map->domain_terms = vocab.extract_domain_terms_from_files(files);

        map->note =
            "Use EntityIDs with get_object_context for full navigation.";
        response.repository_map = std::move(map);
    }

    if (params.include.health_dashboard) {
        auto health = std::make_unique<HealthDashboard>();
        HealthAnalyzer ha;
        health->complexity = ha.calculate_complexity_from_files(files);
        health->hotspots = ha.identify_hotspots_from_files(files);
        double debt_ratio = ha.calculate_tech_debt_ratio_from_files(files);
        health->technical_debt.ratio = debt_ratio;
        health->technical_debt.estimate =
            HealthAnalyzer::estimate_debt_remediation_time(debt_ratio);
        health->technical_debt.components =
            ha.identify_debt_components(files);
        health->problematic_symbols = ha.identify_problematic_symbols(files);
        health->overall_score =
            HealthAnalyzer::calculate_overall_health_score(
                health->complexity, debt_ratio,
                static_cast<int>(health->problematic_symbols.size()));
        // Count from the FULL smell set, then truncate for display —
        // counting a truncated list contradicted the distribution (D3).
        auto all_smells = ha.calculate_detailed_code_smells(files);
        health->smell_counts =
            HealthAnalyzer::count_smells_by_type(all_smells);
        health->detailed_smells = HealthAnalyzer::sort_and_limit_smells(
            std::move(all_smells), ci_thresholds::kMaxDetailedSmells);
        health->analysis_metadata.analyzed_at =
            std::chrono::system_clock::now();
        health->analysis_metadata.files_analyzed =
            static_cast<int>(files.size());
        response.health_dashboard = std::move(health);
    }

    if (params.include.entry_points) {
        auto ep = std::make_unique<EntryPointsList>();
        *ep = build_entry_points(files, params);
        response.entry_points = std::move(ep);
    }

    return response;
}

// ============================================================================
// Detailed (Tier 2) - sub-analyzers deferred to 7.4c
// ============================================================================

CodebaseIntelligenceResponse CodebaseIntelligenceEngine::build_detailed(
    const CodebaseIntelligenceParams& params,
    const std::vector<FileSymbolData>& files, std::string_view project_root,
    const std::function<std::vector<SymbolID>(SymbolID)>& callees_of) const {
    CodebaseIntelligenceResponse response;

    std::string analysis_type = params.analysis.value_or("modules");

    if (analysis_type == "modules") {
        response.module_analysis =
            ModuleAnalyzer().analyze(files, project_root);
    } else if (analysis_type == "layers") {
        response.layer_analysis = LayerAnalyzer().analyze(files);
    } else if (analysis_type == "features") {
        // Feature clustering needs the reference graph; without a callee
        // lookup (e.g. the plain analyze() dispatch, which has no live index)
        // there are no edges to cluster, so leave feature_analysis unset.
        if (callees_of) {
            response.feature_analysis =
                FeatureAnalyzer().analyze(files, callees_of);
        }
    } else if (analysis_type == "terms") {
        response.domain_terms =
            CIVocabularyAnalyzer().extract_domain_terms_from_files(files);
    }
    return response;
}

// ============================================================================
// Statistics (Tier 3)
// ============================================================================

CodebaseIntelligenceResponse CodebaseIntelligenceEngine::build_statistics(
    const CodebaseIntelligenceParams& /*params*/,
    const std::vector<FileSymbolData>& files, std::string_view project_root,
    double purity_ratio,
    const std::function<std::vector<SymbolID>(SymbolID)>& targets_of) const {
    CodebaseIntelligenceResponse response;

    HealthAnalyzer ha;
    ComplexityMetrics complexity = ha.calculate_complexity_from_files(files);

    // Health dashboard kept for callers that read it via analyze() (mirrors
    // the previous behavior); the STATISTICS section renders from
    // statistics_report below.
    auto health = std::make_unique<HealthDashboard>();
    health->complexity = complexity;
    health->overall_score = HealthAnalyzer::calculate_overall_health_score(
        complexity, ha.calculate_tech_debt_ratio_from_files(files),
        static_cast<int>(ha.identify_problematic_symbols(files).size()));
    health->analysis_metadata.analyzed_at = std::chrono::system_clock::now();
    health->analysis_metadata.files_analyzed = static_cast<int>(files.size());
    response.health_dashboard = std::move(health);

    auto coupling = CouplingAnalyzer().analyze(files, project_root, targets_of);

    StatisticsReport report;
    report.complexity = complexity;
    report.coupling = coupling.coupling;
    report.cohesion = coupling.cohesion;
    report.quality =
        HealthAnalyzer::calculate_quality_from_complexity(complexity);
    // The cc-distribution debt ignored long functions and hot fan-in
    // entirely (audits: debt=0.00 beside 85 long functions); the files-based
    // ratio prices all three.
    report.quality.technical_debt_ratio =
        HealthAnalyzer().calculate_tech_debt_ratio_from_files(files);
    report.purity_ratio = purity_ratio;
    response.statistics_report = std::move(report);

    return response;
}

// ============================================================================
// Unified (all tiers)
// ============================================================================

CodebaseIntelligenceResponse CodebaseIntelligenceEngine::build_unified(
    const CodebaseIntelligenceParams& params,
    const std::vector<FileSymbolData>& files,
    int file_count, int symbol_count) const {
    // Build overview with all components enabled
    CodebaseIntelligenceParams overview_params = params;
    overview_params.mode = "overview";
    overview_params.include.repository_map = true;
    overview_params.include.health_dashboard = true;
    overview_params.include.entry_points = true;

    return build_overview(overview_params, files, file_count, symbol_count);
}

// ============================================================================
// Structure
// ============================================================================

CodebaseIntelligenceResponse CodebaseIntelligenceEngine::build_structure(
    const CodebaseIntelligenceParams& /*params*/,
    const std::vector<FileSymbolData>& files,
    const std::vector<std::string>& file_paths,
    const std::vector<PathAttrId>& file_attrs,
    const PathAttrRegistry& registry,
    std::string_view project_root) const {
    CodebaseIntelligenceResponse response;

    // Set navigation hints so callers know the mode exists.
    response.navigation_hints["explore_directory"] =
        "Use search with filter to see files in a directory";
    response.navigation_hints["focus_area"] =
        "Use mode='structure' with focus='<term>' to filter results";

    // Walk the indexed file paths: full directory census (root + every
    // ancestor dir), count per top-level dir + per extension, categorize
    // (code/tests/config/docs), and track deepest path depth.
    //
    // D4 single-source-of-truth: dir/file/symbol counts here must agree with
    // the overview repository map. The old code stored only top-level segment
    // count as dir_count (chi: 4 vs ~21 real dirs) and copied the handler's
    // functions-only tally into symbol_count (guzzle: 16 vs 1205 symbols).
    // The symbol census now derives from `files` — the same data overview
    // counts — so callers cannot inject a diverging figure.
    absl::flat_hash_map<std::string, int> top_dir_files;
    absl::flat_hash_map<std::string, int> types_count;
    absl::flat_hash_set<std::string> all_dirs;
    StructureAnalysis s;
    s.file_count = static_cast<int>(file_paths.size());
    s.symbol_count = count_all_symbols(files);
    s.function_count = count_functions(files);
    for (size_t idx = 0; idx < file_paths.size(); ++idx) {
        const auto& path = file_paths[idx];
        if (path.empty()) continue;
        std::string rel = path;
        if (!project_root.empty() && rel.rfind(project_root, 0) == 0) {
            rel = rel.substr(project_root.size());
            while (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
        }
        int depth = 0;
        for (char c : rel)
            if (c == '/') ++depth;
        if (depth > s.max_depth) s.max_depth = depth;
        // Every ancestor directory counts once; the project root itself is
        // "." (so a flat corpus reports dirs=1, matching `find . -type d`).
        all_dirs.insert(".");
        for (size_t pos = rel.find('/'); pos != std::string::npos;
             pos = rel.find('/', pos + 1)) {
            all_dirs.insert(rel.substr(0, pos));
        }
        auto slash = rel.find('/');
        std::string top =
            slash == std::string::npos ? "." : rel.substr(0, slash);
        ++top_dir_files[top];
        auto dot = rel.rfind('.');
        if (dot != std::string::npos) ++types_count[rel.substr(dot)];
        // Categorize through the canonical classify_file rule (1:1 FileCategory
        // mapping) instead of loose substring matching. This fixes the review
        // finding where rel.find("/test") wrongly matched "/testing" and
        // rel.find(".md") matched a mid-path ".md". FileCategory::Unknown
        // (no recognized extension: bare README, LICENSE, Makefile, ...) routes
        // to the "other" bucket, matching Go categorizeFile's default return
        // "other" (codebase_intelligence_tools.go:846) — never to code.
        // The stored PathClassifier attribute (index-time, config-aware)
        // decides the tests/docs/example/vendored/generated segments; only
        // production files fall through to the extension-category switch.
        PathAttrId attr =
            idx < file_attrs.size() ? file_attrs[idx] : kFallbackAttr;
        if (attr != kFallbackAttr) {
            // Buckets are keyed by attribute NAME, not by an enum: the set is
            // open, so an attribute this build never heard of still lands
            // somewhere visible instead of being counted as production code.
            std::string_view an = registry.name(attr);
            if (an == "test") { ++s.tests; continue; }
            if (an == "docs") { ++s.docs; continue; }
            if (an == "example") { ++s.example; continue; }
            if (an == "vendored") { ++s.vendored; continue; }
            if (an == "generated") { ++s.generated; continue; }
            ++s.other;
            continue;
        }
        switch (classify_file(rel)) {
            case FileCategory::Test: ++s.tests; break;
            case FileCategory::Documentation: ++s.docs; break;
            case FileCategory::Config: ++s.config; break;
            case FileCategory::Code: ++s.code; break;
            case FileCategory::Unknown: ++s.other; break;
        }
    }
    // Go parity: FileCategories.Other is limitSlice(Other, 10) before emit
    // (codebase_intelligence_tools.go:780). The count-based C++ shape clamps to
    // the same cap so the emitted category figure matches Go's list length.
    if (s.other > 10) s.other = 10;
    s.dir_count = static_cast<int>(all_dirs.size());
    s.types.assign(types_count.begin(), types_count.end());
    std::sort(s.types.begin(), s.types.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    s.top_dirs.assign(top_dir_files.begin(), top_dir_files.end());
    // Directory name breaks file-count ties: top_dir_files is a hash map, so
    // equal-count directories otherwise land in per-process order.
    std::sort(s.top_dirs.begin(), s.top_dirs.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });
    response.structure_analysis = std::move(s);

    return response;
}

// ============================================================================
// Git modes
// ============================================================================

// ============================================================================
// Private helpers
// ============================================================================

std::vector<FunctionSignature>
CodebaseIntelligenceEngine::extract_critical_functions(
    const std::vector<FileSymbolData>& files, int max_results) const {
    struct Scored {
        const EnhancedSymbol* sym;
        std::string path;
        double score;
    };

    std::vector<Scored> candidates;
    for (const auto& f : files) {
        for (const auto* sym : f.symbols) {
            if (!is_function_like(sym->symbol.type)) continue;
            double score = calculate_importance_score(*sym);
            if (score > 0.0) {
                candidates.push_back({sym, f.path, score});
            }
        }
    }

    // Total order before the max_results truncation below: importance scores
    // collide readily across a corpus, and std::sort is not stable, so the
    // surviving head was unstable run to run (Karpathy rule 4).
    std::sort(candidates.begin(), candidates.end(),
              [](const Scored& a, const Scored& b) {
                  if (a.score != b.score) return a.score > b.score;
                  if (a.path != b.path) return a.path < b.path;
                  if (a.sym->symbol.name != b.sym->symbol.name) {
                      return a.sym->symbol.name < b.sym->symbol.name;
                  }
                  return a.sym->symbol.line < b.sym->symbol.line;
              });

    if (max_results > 0 &&
        static_cast<int>(candidates.size()) > max_results) {
        candidates.resize(static_cast<size_t>(max_results));
    }

    std::vector<FunctionSignature> result;
    result.reserve(candidates.size());
    for (const auto& c : candidates) {
        FunctionSignature fs;
        fs.name = c.sym->symbol.name;
        fs.module = c.path;
        fs.signature = c.sym->signature;
        fs.importance_score = c.score;
        fs.referenced_by = static_cast<int>(c.sym->incoming_ref_count);
        fs.is_exported = c.sym->is_exported;
        fs.location = c.path + ":" + std::to_string(c.sym->symbol.line);
        result.push_back(std::move(fs));
    }
    return result;
}

namespace {

// A factory/constructor-shaped name is the canonical front door of a library
// (chi.NewRouter, HandlerStack::create, pocketbase.New) — boost it so the
// public surface leads ENTRY POINTS instead of whatever has the most callers.
bool starts_with_lower(std::string_view name, std::string_view prefix) {
    if (name.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(name[i])) != prefix[i])
            return false;
    }
    return true;
}

bool is_factory_name(std::string_view name) {
    return starts_with_lower(name, "new") || starts_with_lower(name, "create") ||
           starts_with_lower(name, "make") || starts_with_lower(name, "build") ||
           starts_with_lower(name, "from");
}

}  // namespace

EntryPointsList CodebaseIntelligenceEngine::build_entry_points(
    const std::vector<FileSymbolData>& files,
    const CodebaseIntelligenceParams& params) const {
    EntryPointsList result;
    result.confidence = params.entry_point_confidence.empty()
                            ? std::string("heuristic")
                            : params.entry_point_confidence;
    absl::flat_hash_set<std::string_view> pin_set;
    for (const auto& p : params.entry_point_pins) pin_set.insert(p);

    // Track path depth (slash count) per api candidate: root-package exports
    // are the library's front door; deeply nested ones usually aren't. Depth
    // is compared relative to the shallowest candidate, so absolute-path
    // prefixes cancel out.
    std::vector<int> api_depth;
    int min_depth = std::numeric_limits<int>::max();

    for (const auto& f : files) {
        int depth = static_cast<int>(
            std::count(f.path.begin(), f.path.end(), '/'));
        for (const auto* sym : f.symbols) {
            if (sym->symbol.test_scaffold) continue;
            // Pinned CLASS symbols pass the function gate: a Python or TS
            // library's front door is often a class (FastAPI, APIRouter) —
            // an authoritative pin on one must be able to seat it.
            bool pinned_class =
                (sym->symbol.type == SymbolType::Class ||
                 sym->symbol.type == SymbolType::Struct) &&
                pin_set.contains(std::string_view(sym->symbol.name));
            if (!is_function_like(sym->symbol.type) && !pinned_class)
                continue;

            // Cargo build scripts define a main() that is not a program
            // entry point (ripgrep listed `binaries: main (build.rs:1)`).
            bool is_build_script =
                f.path.size() >= 8 &&
                f.path.compare(f.path.size() - 8, 8, "build.rs") == 0;
            bool is_main = !is_build_script &&
                           (sym->symbol.name == "main" ||
                            sym->symbol.name == "Main");

            // An abstract declaration has no body and duplicates its concrete
            // implementation in the entry list — guzzle showed ClientTrait's
            // abstract request() beside Client::request(). (Span-based
            // detection is wrong: legitimate one-line functions exist.)
            if (!is_main &&
                sym->signature.find("abstract ") != std::string::npos)
                continue;
            bool is_api = !is_main && sym->is_exported;
            if (!is_main && !is_api) continue;

            EntryPointDef ep;
            ep.name = sym->symbol.name;
            ep.type = is_main ? "main" : "api";
            ep.location = f.path + ":" + std::to_string(sym->symbol.line);
            ep.signature = sym->signature;
            ep.is_exported = sym->is_exported;
            ep.importance = calculate_importance_score(*sym);
            // Authoritative pins outrank every heuristic signal — enforced
            // structurally in the sort comparator (importance is unbounded,
            // so no additive bonus can guarantee seating); the emitter also
            // exempts pinned entries from trivial-name demotion.
            ep.pinned = pin_set.contains(std::string_view(ep.name));
            if (is_api) {
                // Library-aware additive bonuses: exportedness, factory-name
                // shape. These keep zero-fan-in front doors (callers live in
                // downstream repos, not this one) above incidental exports.
                ep.importance += 1.0;                          // exported
                if (is_factory_name(ep.name)) ep.importance += 2.0;
                api_depth.push_back(depth);
                min_depth = std::min(min_depth, depth);
            }
            result.main_functions.push_back(std::move(ep));
        }
    }

    // Root-package bonus, applied once the shallowest depth is known.
    {
        size_t k = 0;
        for (auto& ep : result.main_functions) {
            if (ep.type != "api") continue;
            int over = api_depth[k++] - min_depth;
            ep.importance += std::max(0.0, 1.5 - 0.5 * over);
        }
    }

    // Rank: main() first (consumers split them onto a `binaries:` sub-line),
    // then exported API by adjusted importance (fan-in + exportedness +
    // factory shape + root proximity) descending, then name.
    // `location` (path:line, unique per symbol) is the final tiebreak so the
    // key is TOTAL — without it, same-name same-importance exports (e.g. a
    // function `add` defined in several files) sort in unspecified order and
    // the emitted ENTRY POINTS list flickers run-to-run (karpathy #4).
    std::sort(result.main_functions.begin(), result.main_functions.end(),
              [](const EntryPointDef& a, const EntryPointDef& b) {
                  bool am = a.type == "main", bm = b.type == "main";
                  if (am != bm) return am;
                  if (a.pinned != b.pinned) return a.pinned;
                  if (a.importance != b.importance)
                      return a.importance > b.importance;
                  if (a.name != b.name) return a.name < b.name;
                  return a.location < b.location;
              });
    return result;
}

}  // namespace lci
