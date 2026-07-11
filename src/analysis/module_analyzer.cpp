#include <lci/analysis/module_analyzer.h>

#include <lci/analysis/coupling_analyzer.h>
#include <lci/core/text.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
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
        mb.coupling_score = 0.3;
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

    // Calculate aggregate metrics.
    double total_cohesion = 0.0;
    double total_coupling = 0.0;
    for (const auto& m : modules) {
        total_cohesion += m.cohesion_score;
        total_coupling += m.coupling_score;
    }

    int count = static_cast<int>(modules.size());
    ModuleAnalysisMetrics metrics;
    metrics.total_modules = count;
    metrics.average_cohesion = (count > 0)
        ? total_cohesion / static_cast<double>(count) : 0.0;
    metrics.average_coupling = (count > 0)
        ? total_coupling / static_cast<double>(count) : 0.0;
    metrics.architectural_score = 0.8;

    ModuleAnalysis result;
    result.modules = std::move(modules);
    result.detection_strategy = "directory_structure";
    result.metrics = metrics;

    return result;
}

}  // namespace lci
