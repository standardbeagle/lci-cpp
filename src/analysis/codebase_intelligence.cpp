#include <lci/analysis/codebase_intelligence.h>

#include <lci/analysis/ci_vocabulary_analyzer.h>
#include <lci/analysis/coupling_analyzer.h>
#include <lci/analysis/feature_analyzer.h>
#include <lci/analysis/health_analyzer.h>
#include <lci/analysis/layer_analyzer.h>
#include <lci/analysis/module_analyzer.h>
#include <lci/analysis/token_budget.h>
#include <lci/reference.h>

#include <algorithm>
#include <cctype>
#include <chrono>
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
    double score = static_cast<double>(sym.incoming_refs.size());

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

    auto start = std::chrono::steady_clock::now();

    CodebaseIntelligenceResponse response;
    if (params.mode == "overview") {
        response = build_overview(params, files, file_count, symbol_count);
    } else if (params.mode == "detailed") {
        response = build_detailed(params, files);
    } else if (params.mode == "statistics") {
        response = build_statistics(params, files);
    } else if (params.mode == "unified") {
        response = build_unified(params, files, file_count, symbol_count);
    } else if (params.mode == "structure") {
        response = build_structure(params, files);
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
        response.repository_map = map.release();
    }

    if (params.include.health_dashboard) {
        auto health = std::make_unique<HealthDashboard>();
        HealthAnalyzer ha;
        health->complexity = ha.calculate_complexity_from_files(files);
        health->hotspots = ha.identify_hotspots_from_files(files);
        health->overall_score =
            HealthAnalyzer::calculate_overall_health_score(
                health->complexity,
                static_cast<int>(files.size()));
        double debt_ratio = ha.calculate_tech_debt_ratio_from_files(files);
        health->technical_debt.ratio = debt_ratio;
        health->technical_debt.estimate =
            HealthAnalyzer::estimate_debt_remediation_time(debt_ratio);
        health->technical_debt.components =
            ha.identify_debt_components(files);
        health->detailed_smells = ha.calculate_detailed_code_smells(files);
        health->smell_counts =
            HealthAnalyzer::count_smells_by_type(health->detailed_smells);
        health->problematic_symbols = ha.identify_problematic_symbols(files);
        health->analysis_metadata.analyzed_at =
            std::chrono::system_clock::now();
        health->analysis_metadata.files_analyzed =
            static_cast<int>(files.size());
        response.health_dashboard = health.release();
    }

    if (params.include.entry_points) {
        auto ep = std::make_unique<EntryPointsList>();
        *ep = build_entry_points(files);
        response.entry_points = ep.release();
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
    double purity_ratio) const {
    CodebaseIntelligenceResponse response;

    HealthAnalyzer ha;
    ComplexityMetrics complexity = ha.calculate_complexity_from_files(files);

    // Health dashboard kept for callers that read it via analyze() (mirrors
    // the previous behavior); the STATISTICS section renders from
    // statistics_report below.
    auto health = std::make_unique<HealthDashboard>();
    health->complexity = complexity;
    health->overall_score = HealthAnalyzer::calculate_overall_health_score(
        complexity, static_cast<int>(files.size()));
    health->analysis_metadata.analyzed_at = std::chrono::system_clock::now();
    health->analysis_metadata.files_analyzed = static_cast<int>(files.size());
    response.health_dashboard = health.release();

    auto coupling = CouplingAnalyzer().analyze(files, project_root);

    StatisticsReport report;
    report.complexity = complexity;
    report.coupling = coupling.coupling;
    report.cohesion = coupling.cohesion;
    report.quality =
        HealthAnalyzer::calculate_quality_from_complexity(complexity);
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
    const std::vector<FileSymbolData>& /*files*/,
    const std::vector<std::string>& file_paths, std::string_view project_root,
    int file_count, int total_functions) const {
    CodebaseIntelligenceResponse response;

    // Set navigation hints so callers know the mode exists.
    response.navigation_hints["explore_directory"] =
        "Use search with filter to see files in a directory";
    response.navigation_hints["focus_area"] =
        "Use mode='structure' with focus='<term>' to filter results";

    // Walk the indexed file paths: count per top-level dir + per extension,
    // categorize (code/tests/config/docs), and track deepest path depth.
    absl::flat_hash_map<std::string, int> top_dir_files;
    absl::flat_hash_map<std::string, int> types_count;
    StructureAnalysis s;
    s.file_count = file_count;
    s.symbol_count = total_functions;
    for (const auto& path : file_paths) {
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
        auto slash = rel.find('/');
        std::string top =
            slash == std::string::npos ? "." : rel.substr(0, slash);
        ++top_dir_files[top];
        auto dot = rel.rfind('.');
        if (dot != std::string::npos) ++types_count[rel.substr(dot)];
        if (rel.find("_test.") != std::string::npos ||
            rel.find("/test") != std::string::npos)
            ++s.tests;
        else if (rel.find(".md") != std::string::npos ||
                 rel.find("README") != std::string::npos)
            ++s.docs;
        else if (rel.find(".json") != std::string::npos ||
                 rel.find(".yaml") != std::string::npos ||
                 rel.find(".yml") != std::string::npos ||
                 rel.find(".toml") != std::string::npos)
            ++s.config;
        else
            ++s.code;
    }
    s.dir_count = static_cast<int>(top_dir_files.size());
    s.types.assign(types_count.begin(), types_count.end());
    std::sort(s.types.begin(), s.types.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    s.top_dirs.assign(top_dir_files.begin(), top_dir_files.end());
    std::sort(s.top_dirs.begin(), s.top_dirs.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
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

    std::sort(candidates.begin(), candidates.end(),
              [](const Scored& a, const Scored& b) {
                  return a.score > b.score;
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
        fs.referenced_by = static_cast<int>(c.sym->incoming_refs.size());
        fs.is_exported = c.sym->is_exported;
        fs.location = c.path + ":" + std::to_string(c.sym->symbol.line);
        result.push_back(std::move(fs));
    }
    return result;
}

EntryPointsList CodebaseIntelligenceEngine::build_entry_points(
    const std::vector<FileSymbolData>& files) const {
    EntryPointsList result;

    for (const auto& f : files) {
        for (const auto* sym : f.symbols) {
            if (!is_function_like(sym->symbol.type)) continue;

            bool is_main = (sym->symbol.name == "main" ||
                            sym->symbol.name == "Main");
            bool is_api = !is_main && sym->is_exported;
            if (!is_main && !is_api) continue;

            EntryPointDef ep;
            ep.name = sym->symbol.name;
            ep.type = is_main ? "main" : "api";
            ep.location = f.path + ":" + std::to_string(sym->symbol.line);
            ep.signature = sym->signature;
            ep.is_exported = sym->is_exported;
            ep.importance = calculate_importance_score(*sym);
            result.main_functions.push_back(std::move(ep));
        }
    }

    // Rank: main() first, then exported API by importance (fan-in etc.)
    // descending, then name. Consumers cap to a top-N for display. (The old
    // code kept the first 10 exported symbols in scan order, burying the
    // actual public surface behind whatever was indexed first.)
    // `location` (path:line, unique per symbol) is the final tiebreak so the
    // key is TOTAL — without it, same-name same-importance exports (e.g. a
    // function `add` defined in several files) sort in unspecified order and
    // the emitted ENTRY POINTS list flickers run-to-run (karpathy #4).
    std::sort(result.main_functions.begin(), result.main_functions.end(),
              [](const EntryPointDef& a, const EntryPointDef& b) {
                  bool am = a.type == "main", bm = b.type == "main";
                  if (am != bm) return am;
                  if (a.importance != b.importance)
                      return a.importance > b.importance;
                  if (a.name != b.name) return a.name < b.name;
                  return a.location < b.location;
              });
    return result;
}

}  // namespace lci
