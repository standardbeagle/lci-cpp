#include <lci/analysis/layer_analyzer.h>
#include <lci/core/text.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <string>

namespace lci {

namespace {

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

bool is_function_like(SymbolType t) {
    return t == SymbolType::Function || t == SymbolType::Method;
}

bool is_class_like(SymbolType t) {
    return t == SymbolType::Class || t == SymbolType::Struct ||
           t == SymbolType::Interface;
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

bool has_layer(const std::vector<ArchitecturalLayer>& layers,
               std::string_view name) {
    for (const auto& l : layers) {
        if (l.name == name) return true;
    }
    return false;
}

bool has_all_layers(const std::vector<ArchitecturalLayer>& layers,
                    const std::vector<std::string_view>& required) {
    for (auto req : required) {
        if (!has_layer(layers, req)) return false;
    }
    return true;
}

// Layer keyword tables used by classify_symbol_to_layer.
struct LayerKeywords {
    std::string_view layer;
    std::vector<std::string_view> keywords;
};

const LayerKeywords kLayerKeywords[] = {
    {"Presentation Layer",
     {"component", "view", "page", "screen", "ui", "widget", "button",
      "input", "modal", "dialog", "layout", "template", "render", "display"}},
    {"Application Layer",
     {"service", "manager", "facade", "application", "app", "controller",
      "handler", "command", "interactor", "usecase"}},
    {"Domain Layer",
     {"domain", "model", "entity", "aggregate", "valueobject", "business",
      "logic", "rule", "constraint", "validation"}},
    {"Data Layer",
     {"repository", "dao", "dataaccess", "persistence", "storage", "database",
      "sql", "mapper", "orm"}},
    {"Infrastructure Layer",
     {"config", "adapter", "driver", "client", "http", "https", "api",
      "logger", "log", "metric", "cache", "queue", "message"}},
    {"Utility Layer",
     {"util", "helper", "tool", "common", "shared", "core", "base"}},
};

}  // namespace

// ---------------------------------------------------------------------------
// Symbol classification
// ---------------------------------------------------------------------------

std::string LayerAnalyzer::classify_symbol_to_layer(const EnhancedSymbol& sym) {
    std::string name = text::ascii_lower(sym.symbol.name);

    // Check symbol type for strong classification.
    if (is_class_like(sym.symbol.type)) {
        if (contains(name, "service") || contains(name, "manager"))
            return "Application Layer";
        if (contains(name, "model") || contains(name, "entity"))
            return "Domain Layer";
        if (contains(name, "repository") || contains(name, "dao"))
            return "Data Layer";
        if (contains(name, "component") || contains(name, "view"))
            return "Presentation Layer";
    }
    if (is_function_like(sym.symbol.type)) {
        if (contains(name, "render"))
            return "Presentation Layer";
        if (contains(name, "save") || contains(name, "load"))
            return "Data Layer";
        if (contains(name, "validate") || contains(name, "compute"))
            return "Domain Layer";
    }

    // Fallback: keyword scan.
    for (const auto& entry : kLayerKeywords) {
        for (auto kw : entry.keywords) {
            if (contains(name, kw)) return std::string(entry.layer);
        }
    }

    return "Utility Layer";
}

// ---------------------------------------------------------------------------
// Pattern detection
// ---------------------------------------------------------------------------

std::vector<LayerPattern> LayerAnalyzer::detect_patterns(
    const std::vector<ArchitecturalLayer>& layers) {

    std::vector<LayerPattern> patterns;

    if (has_all_layers(layers, {"Presentation Layer", "Application Layer",
                                "Domain Layer", "Data Layer"})) {
        patterns.push_back({"Layered Architecture", "Layered Architecture", 0.8, {}});
    }
    if (layers.size() > 5) {
        patterns.push_back({"Microservices Architecture",
                            "Microservices Architecture", 0.8, {}});
    }
    if (has_layer(layers, "Presentation Layer") &&
        has_layer(layers, "Application Layer")) {
        patterns.push_back({"MVC Pattern", "MVC Pattern", 0.8, {}});
    }
    if (has_layer(layers, "Data Layer")) {
        patterns.push_back({"Repository Pattern", "Repository Pattern", 0.8, {}});
    }

    return patterns;
}

// ---------------------------------------------------------------------------
// Main analysis
// ---------------------------------------------------------------------------

LayerAnalysis LayerAnalyzer::analyze(
    const std::vector<FileSymbolData>& files) const {

    // Classify every symbol into a layer.
    absl::flat_hash_map<std::string, std::vector<const EnhancedSymbol*>> layer_map;
    for (const auto& file : files) {
        for (const auto* sym : file.symbols) {
            std::string layer = classify_symbol_to_layer(*sym);
            layer_map[layer].push_back(sym);
        }
    }

    // Build ArchitecturalLayer entries.
    std::vector<ArchitecturalLayer> layers;
    int depth = 1;
    for (auto& [name, syms] : layer_map) {
        if (syms.empty()) continue;

        std::vector<std::string> module_names;
        module_names.reserve(syms.size());
        for (const auto* sym : syms) {
            module_names.push_back(sym->symbol.name);
        }

        LayerMetricsData m;
        m.module_count = static_cast<int>(syms.size());
        m.cohesion_score = prefix_cohesion(syms);
        m.coupling_score = 0.3;
        m.maintainability = 80.0;
        m.complexity = static_cast<double>(syms.size()) / 10.0;

        ArchitecturalLayer al;
        al.name = name;
        al.modules = std::move(module_names);
        al.depth = depth++;
        al.component_types = {name};
        al.metrics = m;
        layers.push_back(std::move(al));
    }

    // Detect violations.
    int violation_count = 0;
    if (!layers.empty()) {
        int max_size = 0;
        int min_size = INT_MAX;
        for (const auto& l : layers) {
            if (l.metrics.module_count > max_size) max_size = l.metrics.module_count;
            if (l.metrics.module_count < min_size) min_size = l.metrics.module_count;
        }
        if (min_size > 0 && max_size > min_size * 5) ++violation_count;
    }

    static const std::string_view expected[] = {
        "Presentation Layer", "Application Layer", "Domain Layer", "Data Layer"};
    for (auto exp : expected) {
        if (!has_layer(layers, exp)) ++violation_count;
    }

    // Build simple dependency matrix and metrics.
    std::vector<LayerMetricsData> layer_metrics;
    layer_metrics.reserve(layers.size());
    for (const auto& l : layers) {
        layer_metrics.push_back(l.metrics);
    }

    auto patterns = detect_patterns(layers);

    std::vector<std::vector<double>> dep_matrix = {
        {1.0, 0.5, 0.3, 0.2},
        {0.5, 1.0, 0.7, 0.4},
        {0.3, 0.7, 1.0, 0.6},
        {0.2, 0.4, 0.6, 1.0},
    };

    LayerAnalysis result;
    result.layers = std::move(layers);
    result.violation_count = violation_count;
    result.layer_metrics = std::move(layer_metrics);
    result.patterns = std::move(patterns);
    result.dependency_matrix = std::move(dep_matrix);

    return result;
}

}  // namespace lci
