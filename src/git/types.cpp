#include <lci/git/types.h>

#include <algorithm>
#include <string>

namespace lci {
namespace git {

namespace {

/// Static naming convention data, built once.
struct ConventionTable {
    absl::flat_hash_map<Language, NamingConvention> conventions;

    ConventionTable() {
        // Go
        {
            NamingConvention c;
            c.description = "Go uses PascalCase for exported, camelCase for unexported";
            auto both = std::vector<CaseStyle>{CaseStyle::PascalCase, CaseStyle::CamelCase};
            for (auto k : {SymbolKind::Function, SymbolKind::Method, SymbolKind::Class,
                           SymbolKind::Interface, SymbolKind::Struct, SymbolKind::Type,
                           SymbolKind::Constant, SymbolKind::Variable, SymbolKind::Field,
                           SymbolKind::Enum, SymbolKind::EnumMember}) {
                c.expected_styles[k] = both;
            }
            conventions[Language::Go] = std::move(c);
        }
        // JavaScript
        {
            NamingConvention c;
            c.description = "JavaScript uses camelCase for functions/variables, PascalCase for classes";
            auto camel = std::vector<CaseStyle>{CaseStyle::CamelCase};
            c.expected_styles[SymbolKind::Function] = camel;
            c.expected_styles[SymbolKind::Method] = camel;
            c.expected_styles[SymbolKind::Class] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Interface] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Constant] = {CaseStyle::CamelCase, CaseStyle::SnakeCase};
            c.expected_styles[SymbolKind::Variable] = camel;
            c.expected_styles[SymbolKind::Field] = camel;
            c.expected_styles[SymbolKind::Property] = camel;
            conventions[Language::JavaScript] = std::move(c);
        }
        // TypeScript
        {
            NamingConvention c;
            c.description = "TypeScript uses camelCase for functions/variables, PascalCase for types/classes";
            auto camel = std::vector<CaseStyle>{CaseStyle::CamelCase};
            c.expected_styles[SymbolKind::Function] = camel;
            c.expected_styles[SymbolKind::Method] = camel;
            c.expected_styles[SymbolKind::Class] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Interface] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Type] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Constant] = {CaseStyle::CamelCase, CaseStyle::SnakeCase};
            c.expected_styles[SymbolKind::Variable] = camel;
            c.expected_styles[SymbolKind::Field] = camel;
            c.expected_styles[SymbolKind::Property] = camel;
            c.expected_styles[SymbolKind::Enum] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::EnumMember] = {CaseStyle::PascalCase, CaseStyle::SnakeCase};
            conventions[Language::TypeScript] = std::move(c);
        }
        // Python
        {
            NamingConvention c;
            c.description = "Python uses snake_case for functions/variables, PascalCase for classes";
            auto snake = std::vector<CaseStyle>{CaseStyle::SnakeCase};
            c.expected_styles[SymbolKind::Function] = snake;
            c.expected_styles[SymbolKind::Method] = snake;
            c.expected_styles[SymbolKind::Class] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Constant] = snake;
            c.expected_styles[SymbolKind::Variable] = snake;
            c.expected_styles[SymbolKind::Field] = snake;
            c.expected_styles[SymbolKind::Property] = snake;
            c.expected_styles[SymbolKind::Module] = snake;
            conventions[Language::Python] = std::move(c);
        }
        // Rust
        {
            NamingConvention c;
            c.description = "Rust uses snake_case for functions/variables, PascalCase for types";
            auto snake = std::vector<CaseStyle>{CaseStyle::SnakeCase};
            c.expected_styles[SymbolKind::Function] = snake;
            c.expected_styles[SymbolKind::Method] = snake;
            c.expected_styles[SymbolKind::Class] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Interface] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Struct] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Type] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Constant] = snake;
            c.expected_styles[SymbolKind::Variable] = snake;
            c.expected_styles[SymbolKind::Field] = snake;
            c.expected_styles[SymbolKind::Enum] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::EnumMember] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Module] = snake;
            conventions[Language::Rust] = std::move(c);
        }
        // Java
        {
            NamingConvention c;
            c.description = "Java uses camelCase for methods/variables, PascalCase for classes";
            auto camel = std::vector<CaseStyle>{CaseStyle::CamelCase};
            c.expected_styles[SymbolKind::Function] = camel;
            c.expected_styles[SymbolKind::Method] = camel;
            c.expected_styles[SymbolKind::Class] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Interface] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Constant] = {CaseStyle::SnakeCase};
            c.expected_styles[SymbolKind::Variable] = camel;
            c.expected_styles[SymbolKind::Field] = camel;
            c.expected_styles[SymbolKind::Enum] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::EnumMember] = {CaseStyle::SnakeCase};
            conventions[Language::Java] = std::move(c);
        }
        // C#
        {
            NamingConvention c;
            c.description = "C# uses PascalCase for public members, camelCase for private";
            c.expected_styles[SymbolKind::Function] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Method] = {CaseStyle::PascalCase, CaseStyle::CamelCase};
            c.expected_styles[SymbolKind::Class] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Interface] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Struct] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Constant] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Variable] = {CaseStyle::CamelCase};
            c.expected_styles[SymbolKind::Field] = {CaseStyle::CamelCase, CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Property] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Enum] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::EnumMember] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Namespace] = {CaseStyle::PascalCase};
            conventions[Language::CSharp] = std::move(c);
        }
        // C++
        {
            NamingConvention c;
            c.description = "C++ conventions vary, commonly snake_case or camelCase for functions";
            c.expected_styles[SymbolKind::Function] = {CaseStyle::SnakeCase, CaseStyle::CamelCase, CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Method] = {CaseStyle::SnakeCase, CaseStyle::CamelCase, CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Class] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Struct] = {CaseStyle::PascalCase, CaseStyle::SnakeCase};
            c.expected_styles[SymbolKind::Constant] = {CaseStyle::SnakeCase};
            c.expected_styles[SymbolKind::Variable] = {CaseStyle::SnakeCase, CaseStyle::CamelCase};
            c.expected_styles[SymbolKind::Field] = {CaseStyle::SnakeCase, CaseStyle::CamelCase};
            c.expected_styles[SymbolKind::Namespace] = {CaseStyle::SnakeCase, CaseStyle::PascalCase};
            conventions[Language::Cpp] = std::move(c);
        }
        // C
        {
            NamingConvention c;
            c.description = "C uses snake_case for functions/variables";
            auto snake = std::vector<CaseStyle>{CaseStyle::SnakeCase};
            c.expected_styles[SymbolKind::Function] = snake;
            c.expected_styles[SymbolKind::Struct] = {CaseStyle::SnakeCase, CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Constant] = snake;
            c.expected_styles[SymbolKind::Variable] = snake;
            c.expected_styles[SymbolKind::Field] = snake;
            conventions[Language::C] = std::move(c);
        }
        // Ruby
        {
            NamingConvention c;
            c.description = "Ruby uses snake_case for methods/variables, PascalCase for classes";
            auto snake = std::vector<CaseStyle>{CaseStyle::SnakeCase};
            c.expected_styles[SymbolKind::Function] = snake;
            c.expected_styles[SymbolKind::Method] = snake;
            c.expected_styles[SymbolKind::Class] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Module] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Constant] = snake;
            c.expected_styles[SymbolKind::Variable] = snake;
            c.expected_styles[SymbolKind::Field] = snake;
            conventions[Language::Ruby] = std::move(c);
        }
        // Swift
        {
            NamingConvention c;
            c.description = "Swift uses camelCase for functions/properties, PascalCase for types";
            auto camel = std::vector<CaseStyle>{CaseStyle::CamelCase};
            c.expected_styles[SymbolKind::Function] = camel;
            c.expected_styles[SymbolKind::Method] = camel;
            c.expected_styles[SymbolKind::Class] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Interface] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Struct] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Type] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Constant] = camel;
            c.expected_styles[SymbolKind::Variable] = camel;
            c.expected_styles[SymbolKind::Property] = camel;
            c.expected_styles[SymbolKind::Enum] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::EnumMember] = camel;
            conventions[Language::Swift] = std::move(c);
        }
        // Kotlin
        {
            NamingConvention c;
            c.description = "Kotlin uses camelCase for functions/properties, PascalCase for classes";
            auto camel = std::vector<CaseStyle>{CaseStyle::CamelCase};
            c.expected_styles[SymbolKind::Function] = camel;
            c.expected_styles[SymbolKind::Method] = camel;
            c.expected_styles[SymbolKind::Class] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Interface] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Constant] = {CaseStyle::SnakeCase};
            c.expected_styles[SymbolKind::Variable] = camel;
            c.expected_styles[SymbolKind::Property] = camel;
            c.expected_styles[SymbolKind::Enum] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::EnumMember] = {CaseStyle::SnakeCase};
            conventions[Language::Kotlin] = std::move(c);
        }
        // Scala
        {
            NamingConvention c;
            c.description = "Scala uses camelCase for methods/values, PascalCase for types";
            auto camel = std::vector<CaseStyle>{CaseStyle::CamelCase};
            c.expected_styles[SymbolKind::Function] = camel;
            c.expected_styles[SymbolKind::Method] = camel;
            c.expected_styles[SymbolKind::Class] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Interface] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Type] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Constant] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Variable] = camel;
            conventions[Language::Scala] = std::move(c);
        }
        // Zig
        {
            NamingConvention c;
            c.description = "Zig uses camelCase for functions, PascalCase for types";
            c.expected_styles[SymbolKind::Function] = {CaseStyle::CamelCase};
            c.expected_styles[SymbolKind::Struct] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Type] = {CaseStyle::PascalCase};
            c.expected_styles[SymbolKind::Constant] = {CaseStyle::SnakeCase};
            c.expected_styles[SymbolKind::Variable] = {CaseStyle::SnakeCase, CaseStyle::CamelCase};
            conventions[Language::Zig] = std::move(c);
        }
    }
};

const ConventionTable& convention_table() {
    static const ConventionTable table;
    return table;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
}

}  // namespace

Language get_language_from_path(std::string_view path) {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return Language::Unknown;
    auto ext = path.substr(dot);

    if (ext == ".go") return Language::Go;
    if (ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".cjs") return Language::JavaScript;
    if (ext == ".ts" || ext == ".tsx" || ext == ".mts" || ext == ".cts") return Language::TypeScript;
    if (ext == ".py" || ext == ".pyw" || ext == ".pyi" || ext == ".pyx" ||
        ext == ".pxd")
        return Language::Python;
    if (ext == ".rs") return Language::Rust;
    if (ext == ".java") return Language::Java;
    if (ext == ".cs") return Language::CSharp;
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".hpp" || ext == ".hxx" || ends_with(ext, ".h++")) return Language::Cpp;
    if (ext == ".c" || ext == ".h") return Language::C;
    if (ext == ".php") return Language::PHP;
    if (ext == ".rb") return Language::Ruby;
    if (ext == ".swift") return Language::Swift;
    if (ext == ".kt" || ext == ".kts") return Language::Kotlin;
    if (ext == ".scala" || ext == ".sc") return Language::Scala;
    if (ext == ".zig") return Language::Zig;
    return Language::Unknown;
}

SymbolKind symbol_type_to_kind(std::string_view symbol_type) {
    if (symbol_type == "function") return SymbolKind::Function;
    if (symbol_type == "method") return SymbolKind::Method;
    if (symbol_type == "class") return SymbolKind::Class;
    if (symbol_type == "interface") return SymbolKind::Interface;
    if (symbol_type == "struct") return SymbolKind::Struct;
    if (symbol_type == "type" || symbol_type == "type_alias") return SymbolKind::Type;
    if (symbol_type == "constant") return SymbolKind::Constant;
    if (symbol_type == "variable") return SymbolKind::Variable;
    if (symbol_type == "field") return SymbolKind::Field;
    if (symbol_type == "enum") return SymbolKind::Enum;
    if (symbol_type == "enum_member") return SymbolKind::EnumMember;
    if (symbol_type == "module") return SymbolKind::Module;
    if (symbol_type == "namespace") return SymbolKind::Namespace;
    if (symbol_type == "property") return SymbolKind::Property;
    return SymbolKind::UnknownKind;
}

const NamingConvention* get_naming_convention(Language lang) {
    auto& table = convention_table();
    auto it = table.conventions.find(lang);
    if (it == table.conventions.end()) return nullptr;
    return &it->second;
}

bool is_valid_case_style(Language lang, SymbolKind kind, CaseStyle style) {
    const auto* conv = get_naming_convention(lang);
    if (!conv) return true;

    auto it = conv->expected_styles.find(kind);
    if (it == conv->expected_styles.end()) return true;

    for (auto expected : it->second) {
        if (style == expected) return true;
    }
    return false;
}

std::vector<CaseStyle> get_expected_styles(Language lang, SymbolKind kind) {
    const auto* conv = get_naming_convention(lang);
    if (!conv) return {};

    auto it = conv->expected_styles.find(kind);
    if (it == conv->expected_styles.end()) return {};
    return it->second;
}

CaseStyle detect_case_style(std::string_view name) {
    if (name.empty()) return CaseStyle::Unknown;

    bool has_underscore = false;
    bool has_hyphen = false;
    bool has_upper_start = (name[0] >= 'A' && name[0] <= 'Z');
    bool has_lower_after_upper = false;
    bool has_upper_after_lower = false;

    for (size_t i = 0; i < name.size(); ++i) {
        char ch = name[i];
        if (ch == '_') has_underscore = true;
        if (ch == '-') has_hyphen = true;
        if (i > 0) {
            char prev = name[i - 1];
            if (prev >= 'a' && prev <= 'z' && ch >= 'A' && ch <= 'Z') {
                has_upper_after_lower = true;
            }
            if (prev >= 'A' && prev <= 'Z' && ch >= 'a' && ch <= 'z') {
                has_lower_after_upper = true;
            }
        }
    }

    if (has_underscore) return CaseStyle::SnakeCase;
    if (has_hyphen) return CaseStyle::KebabCase;
    if (has_upper_start && (has_lower_after_upper || has_upper_after_lower)) return CaseStyle::PascalCase;
    if (!has_upper_start && has_upper_after_lower) return CaseStyle::CamelCase;
    return CaseStyle::Unknown;
}

// ============================================================================
// Risk / Severity Calculations (from results.go)
// ============================================================================

double calculate_risk_score(const std::vector<DuplicateFinding>& duplicates,
                            const std::vector<NamingFinding>& naming_issues,
                            const std::vector<MetricsFinding>& metrics_issues) {
    double risk = 0.0;

    for (const auto& dup : duplicates) {
        switch (dup.severity) {
            case FindingSeverity::Critical: risk += 0.15; break;
            case FindingSeverity::Warning: risk += 0.08; break;
            case FindingSeverity::Info: risk += 0.03; break;
        }
    }

    for (const auto& issue : naming_issues) {
        switch (issue.severity) {
            case FindingSeverity::Critical: risk += 0.10; break;
            case FindingSeverity::Warning: risk += 0.05; break;
            case FindingSeverity::Info: risk += 0.02; break;
        }
    }

    for (const auto& issue : metrics_issues) {
        switch (issue.severity) {
            case FindingSeverity::Critical: risk += 0.12; break;
            case FindingSeverity::Warning: risk += 0.06; break;
            case FindingSeverity::Info: risk += 0.02; break;
        }
    }

    return std::min(risk, 1.0);
}

FindingSeverity determine_duplicate_severity(double similarity, int line_count) {
    if (similarity >= 0.95 && line_count >= 20) return FindingSeverity::Critical;
    if (similarity >= 0.90 || line_count >= 30) return FindingSeverity::Warning;
    return FindingSeverity::Info;
}

FindingSeverity determine_naming_severity(NamingIssueType issue_type, double similarity) {
    switch (issue_type) {
        case NamingIssueType::SimilarExists:
            return (similarity >= 0.9) ? FindingSeverity::Warning : FindingSeverity::Info;
        case NamingIssueType::CaseMismatch:
            return FindingSeverity::Warning;
        case NamingIssueType::Abbreviation:
            return FindingSeverity::Info;
    }
    return FindingSeverity::Info;
}

FindingSeverity determine_metrics_severity(MetricsIssueType issue_type,
                                           const SymbolMetrics& metrics,
                                           const MetricsThresholds& thresholds) {
    switch (issue_type) {
        case MetricsIssueType::HighComplexity:
            return (metrics.complexity > thresholds.high_complexity * 2)
                       ? FindingSeverity::Critical
                       : FindingSeverity::Warning;
        case MetricsIssueType::LongFunction:
            return (metrics.lines_of_code > thresholds.long_function * 2)
                       ? FindingSeverity::Critical
                       : FindingSeverity::Warning;
        case MetricsIssueType::DeepNesting:
            return (metrics.nesting_depth > thresholds.deep_nesting + 2)
                       ? FindingSeverity::Critical
                       : FindingSeverity::Warning;
        case MetricsIssueType::ComplexityGrew:
            return FindingSeverity::Warning;
        case MetricsIssueType::PurityLost:
            return FindingSeverity::Warning;
        case MetricsIssueType::ImpureFunction:
            return FindingSeverity::Info;
    }
    return FindingSeverity::Info;
}

std::string generate_top_recommendation(const std::vector<DuplicateFinding>& duplicates,
                                        const std::vector<NamingFinding>& naming_issues,
                                        const std::vector<MetricsFinding>& metrics_issues) {
    for (const auto& dup : duplicates) {
        if (dup.severity == FindingSeverity::Critical) return dup.suggestion;
    }
    for (const auto& issue : metrics_issues) {
        if (issue.severity == FindingSeverity::Critical) return issue.suggestion;
    }
    for (const auto& issue : naming_issues) {
        if (issue.severity == FindingSeverity::Critical) return issue.suggestion;
    }
    if (!duplicates.empty()) return duplicates[0].suggestion;
    if (!metrics_issues.empty()) return metrics_issues[0].suggestion;
    if (!naming_issues.empty()) return naming_issues[0].suggestion;
    return {};
}

}  // namespace git
}  // namespace lci
