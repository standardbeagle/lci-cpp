#include <lci/analysis/feature_analyzer.h>

#include <algorithm>
#include <cctype>
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

bool contains_any(std::string_view s, const std::vector<std::string_view>& subs) {
    for (auto sub : subs) {
        if (contains(s, sub)) return true;
    }
    return false;
}

// Feature keyword patterns for grouping symbols.
const std::vector<std::string_view> kFeaturePatterns = {
    "user", "auth", "login", "register", "account", "profile",
    "order", "cart", "checkout", "payment", "billing", "invoice",
    "product", "catalog", "inventory", "stock", "price",
    "notification", "email", "sms", "push", "alert",
    "report", "analytics", "dashboard", "metric", "statistic",
    "search", "filter", "query", "sort", "pagination",
    "upload", "download", "file", "image", "document",
    "config", "setting", "preference", "option",
    "api", "endpoint", "service", "controller",
    "database", "db", "sql", "cache", "session",
};

bool is_function_like(SymbolType t) {
    return t == SymbolType::Function || t == SymbolType::Method;
}

bool is_class_like(SymbolType t) {
    return t == SymbolType::Class || t == SymbolType::Struct;
}

// Calculate prefix-based cohesion score.
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

// Calculate completeness based on required component types.
double feature_completeness(const std::vector<const EnhancedSymbol*>& syms) {
    if (syms.empty()) return 0.0;

    static const std::string_view required[] = {
        "Controller", "Service", "Repository", "Model"};

    absl::flat_hash_map<std::string, bool> present;
    for (const auto* sym : syms) {
        present[FeatureAnalyzer::classify_component_type(*sym)] = true;
    }

    int matches = 0;
    for (auto req : required) {
        if (present.contains(std::string(req))) ++matches;
    }
    return static_cast<double>(matches) / 4.0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Component / feature classification
// ---------------------------------------------------------------------------

std::string FeatureAnalyzer::classify_component_type(const EnhancedSymbol& sym) {
    std::string name = to_lower(sym.symbol.name);

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
    std::string lower = to_lower(name);

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
// Main analysis
// ---------------------------------------------------------------------------

FeatureAnalysis FeatureAnalyzer::analyze(
    const std::vector<FileSymbolData>& files) const {

    // Group symbols by feature pattern.
    absl::flat_hash_map<std::string, std::vector<const EnhancedSymbol*>> groups;

    for (const auto& file : files) {
        for (const auto* sym : file.symbols) {
            std::string sym_lower = to_lower(sym->symbol.name);
            std::string matched;

            // Match by symbol name first.
            for (auto pattern : kFeaturePatterns) {
                if (contains(sym_lower, pattern)) {
                    matched = std::string(pattern);
                    break;
                }
            }

            // Fallback: match by directory base name.
            if (matched.empty()) {
                std::string dir_base = to_lower(
                    std::filesystem::path(file.path).parent_path().filename().string());
                for (auto pattern : kFeaturePatterns) {
                    if (contains(dir_base, pattern)) {
                        matched = std::string(pattern);
                        break;
                    }
                }
            }

            if (matched.empty()) matched = "general";
            groups[matched].push_back(sym);
        }
    }

    // Convert groups to Feature list.
    std::vector<Feature> features;
    double total_cohesion = 0.0;
    double total_complexity = 0.0;

    for (auto& [name, syms] : groups) {
        if (syms.empty()) continue;

        std::vector<ComponentInfo> components;
        components.reserve(syms.size());
        for (const auto* sym : syms) {
            ComponentInfo ci;
            ci.name = sym->symbol.name;
            ci.type = classify_component_type(*sym);
            ci.complexity = static_cast<double>(sym->symbol.name.size()) / 10.0;
            components.push_back(std::move(ci));
        }

        double coh = prefix_cohesion(syms);
        double cmplx = static_cast<double>(syms.size()) / 10.0;
        total_cohesion += coh;
        total_complexity += cmplx;

        Feature f;
        f.name = name;
        f.primary_module = classify_feature_type(name);
        f.components = std::move(components);
        f.confidence = 0.8;
        features.push_back(std::move(f));
    }

    int count = static_cast<int>(features.size());
    double avg_components = 0.0;
    if (count > 0) {
        int total_comp = 0;
        for (const auto& f : features) {
            total_comp += static_cast<int>(f.components.size());
        }
        avg_components = static_cast<double>(total_comp) / static_cast<double>(count);
    }

    FeatureAnalysis result;
    result.features = std::move(features);
    result.metrics.total_features = count;
    result.metrics.average_components = avg_components;
    result.metrics.coupling_score = (count > 0)
        ? total_cohesion / static_cast<double>(count) : 0.0;
    result.metrics.modularity_score = (count > 0)
        ? total_complexity / static_cast<double>(count) : 0.0;

    return result;
}

}  // namespace lci
