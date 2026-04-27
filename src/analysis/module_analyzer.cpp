#include <lci/analysis/module_analyzer.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>

namespace lci {

namespace {

std::string to_lower(std::string_view s) {
    std::string result(s);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

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
            ? to_lower(sym->symbol.name.substr(0, pos))
            : to_lower(sym->symbol.name);
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
    std::string lower = to_lower(path);

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
    const std::vector<FileSymbolData>& files) const {

    // Group symbols by directory.
    absl::flat_hash_map<std::string, std::vector<const EnhancedSymbol*>> dir_groups;
    absl::flat_hash_map<std::string, absl::flat_hash_map<std::string, bool>> dir_files;

    for (const auto& file : files) {
        auto dir = std::filesystem::path(file.path).parent_path().string();
        if (dir.empty() || dir == ".") dir = "root";

        for (const auto* sym : file.symbols) {
            dir_groups[dir].push_back(sym);
            dir_files[dir][file.path] = true;
        }
    }

    // Build module boundaries.
    std::vector<ModuleBoundary> modules;
    for (auto& [dir, syms] : dir_groups) {
        std::string mod_name = std::filesystem::path(dir).filename().string();
        if (mod_name.empty() || mod_name == ".") mod_name = "root";

        double coh = prefix_cohesion(syms);
        double stab = stability_score(static_cast<int>(syms.size()));
        int file_count = static_cast<int>(dir_files[dir].size());

        ModuleBoundary mb;
        mb.name = mod_name;
        mb.type = classify_module_by_path(dir);
        mb.path = dir;
        mb.cohesion_score = coh;
        mb.coupling_score = 0.3;
        mb.stability = stab;
        mb.file_count = file_count;
        mb.function_count = static_cast<int>(syms.size());
        modules.push_back(std::move(mb));
    }

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
