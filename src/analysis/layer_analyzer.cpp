#include <lci/analysis/coupling_analyzer.h>
#include <lci/analysis/layer_analyzer.h>
#include <lci/core/text.h>

#include <algorithm>
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
    // Confidence is MEASURED: the share of classified symbols that live in
    // the layers the pattern requires. The previous constant 0.80 on four
    // simultaneous patterns (Microservices included, for any corpus with
    // >5 layers) was a fabricated prior, not a detection. Patterns below
    // the floor are noise from incidental keyword hits and are not
    // reported at all.
    constexpr double kConfidenceFloor = 0.5;

    int total_symbols = 0;
    for (const auto& l : layers) total_symbols += l.metrics.symbol_count;
    if (total_symbols == 0) return {};

    auto symbol_share = [&](const std::vector<std::string_view>& names) {
        int n = 0;
        for (const auto& l : layers) {
            for (auto name : names) {
                if (l.name == name) n += l.metrics.symbol_count;
            }
        }
        return static_cast<double>(n) / static_cast<double>(total_symbols);
    };

    std::vector<LayerPattern> patterns;
    static const std::vector<std::string_view> kCore = {
        "Presentation Layer", "Application Layer", "Domain Layer",
        "Data Layer"};
    if (has_all_layers(layers, kCore)) {
        double confidence = symbol_share(kCore);
        if (confidence >= kConfidenceFloor) {
            patterns.push_back({"Layered Architecture",
                                "Presentation/Application/Domain/Data "
                                "vocabulary covers most symbols",
                                confidence,
                                {}});
        }
    }
    return patterns;
}

// ---------------------------------------------------------------------------
// Main analysis
// ---------------------------------------------------------------------------

LayerAnalysis LayerAnalyzer::analyze(const std::vector<FileSymbolData>& files,
                                     std::string_view project_root) const {
    // Classify every symbol into a layer, tracking each symbol's module
    // (package directory, same naming as ModuleAnalyzer/CouplingAnalyzer).
    // A layer's `modules` are MODULES — the earlier emission of one entry
    // per symbol reported "modules=440917" on a repo with 817 modules.
    absl::flat_hash_map<std::string, std::vector<const EnhancedSymbol*>>
        layer_symbols;
    absl::flat_hash_map<std::string,
                        absl::flat_hash_map<std::string, int>>
        module_layer_votes;  // module -> layer -> symbol count
    for (const auto& file : files) {
        std::string module =
            CouplingAnalyzer::get_package_name(file.path, project_root);
        for (const auto* sym : file.symbols) {
            std::string layer = classify_symbol_to_layer(*sym);
            layer_symbols[layer].push_back(sym);
            module_layer_votes[module][layer]++;
        }
    }

    // Each module belongs to exactly one layer: majority vote of its
    // symbols' classifications, ties broken by layer name for determinism.
    absl::flat_hash_map<std::string, std::vector<std::string>> layer_modules;
    for (const auto& [module, votes] : module_layer_votes) {
        std::string best_layer;
        int best_votes = -1;
        for (const auto& [layer, n] : votes) {
            if (n > best_votes ||
                (n == best_votes && layer < best_layer)) {
                best_layer = layer;
                best_votes = n;
            }
        }
        layer_modules[best_layer].push_back(module);
    }

    // Deterministic layer order (Karpathy rule 4). The maps above are absl
    // hash maps, so without this both the emitted order AND the `depth`
    // each layer is assigned come out of a per-process hash seed.
    std::vector<std::string> layer_names;
    layer_names.reserve(layer_symbols.size());
    for (const auto& [name, _] : layer_symbols) layer_names.push_back(name);
    std::sort(layer_names.begin(), layer_names.end());

    std::vector<ArchitecturalLayer> layers;
    layers.reserve(layer_names.size());
    int depth = 1;
    for (const auto& name : layer_names) {
        auto& syms = layer_symbols[name];
        if (syms.empty()) continue;

        auto modules = std::move(layer_modules[name]);
        std::sort(modules.begin(), modules.end());

        LayerMetricsData m;
        m.module_count = static_cast<int>(modules.size());
        m.symbol_count = static_cast<int>(syms.size());
        m.cohesion_score = prefix_cohesion(syms);

        ArchitecturalLayer al;
        al.name = name;
        al.modules = std::move(modules);
        al.depth = depth++;
        al.component_types = {name};
        al.metrics = m;
        layers.push_back(std::move(al));
    }

    LayerAnalysis result;
    result.patterns = detect_patterns(layers);
    result.layers = std::move(layers);
    return result;
}

}  // namespace lci
