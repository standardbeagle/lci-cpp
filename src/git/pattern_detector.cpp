#include <lci/git/pattern_detector.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <regex>
#include <string>

namespace lci {
namespace git {

namespace {

bool str_contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

bool str_ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
}

std::string to_lower(std::string_view s) {
    std::string result(s);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

int count_regex_matches(std::string_view content, const std::regex& re) {
    auto begin = std::cregex_iterator(content.data(), content.data() + content.size(), re);
    auto end = std::cregex_iterator();
    return static_cast<int>(std::distance(begin, end));
}

int count_newlines(std::string_view s) {
    return static_cast<int>(std::count(s.begin(), s.end(), '\n'));
}

/// Finds the containing function name and line range for a regex pattern match.
std::pair<std::string, std::string> find_containing_function(
    std::string_view content, const std::regex& pattern) {
    auto begin = std::cregex_iterator(content.data(), content.data() + content.size(), pattern);
    if (begin == std::cregex_iterator()) return {{}, {}};

    auto first_pos = static_cast<size_t>(begin->position());
    auto prefix = content.substr(0, first_pos);

    static const std::regex func_re(R"(^func\s+(\w+)\s*\()");
    std::string func_name;
    int start_line = 0;

    auto func_begin = std::cregex_iterator(prefix.data(), prefix.data() + prefix.size(), func_re);
    auto func_end = std::cregex_iterator();

    std::cmatch last_match;
    for (auto it = func_begin; it != func_end; ++it) {
        last_match = *it;
    }

    if (!last_match.empty() && last_match.size() >= 2) {
        func_name = last_match[1].str();
        start_line = count_newlines(prefix.substr(0, static_cast<size_t>(last_match.position()))) + 1;
        int end_line = start_line + 100;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "lines %d-%d", start_line, end_line);
        return {func_name, std::string(buf)};
    }

    return {{}, {}};
}

}  // namespace

PatternDetector::PatternDetector() = default;

std::vector<AntiPattern> PatternDetector::detect_patterns(
    std::string_view content, std::string_view file_path) const {
    std::vector<AntiPattern> patterns;

    AntiPattern ap;
    if (detect_registration_function(content, file_path, ap)) {
        patterns.push_back(std::move(ap));
    }
    if (detect_enum_aggregation(content, file_path, ap)) {
        patterns.push_back(std::move(ap));
    }
    if (detect_god_object(content, file_path, ap)) {
        patterns.push_back(std::move(ap));
    }
    auto switches = detect_switch_factories(content, file_path);
    patterns.insert(patterns.end(),
                    std::make_move_iterator(switches.begin()),
                    std::make_move_iterator(switches.end()));
    if (detect_barrel_file(content, file_path, ap)) {
        patterns.push_back(std::move(ap));
    }
    if (detect_config_aggregation(content, file_path, ap)) {
        patterns.push_back(std::move(ap));
    }

    return patterns;
}

bool PatternDetector::quick_scan(std::string_view content,
                                 std::string_view file_path,
                                 AntiPattern& out) const {
    int line_count = count_lines(content);
    if (line_count >= god_object_lines_threshold) {
        out.type = AntiPatternType::GodObject;
        out.description = "Large file with " + std::to_string(line_count) + " lines";
        out.location = "entire file";
        out.severity = AntiPatternSeverity::High;
        out.suggestion = "Consider splitting into smaller, focused modules";
        out.metrics.clear();
        return true;
    }
    return false;
}

bool PatternDetector::detect_registration_function(
    std::string_view content, std::string_view file_path,
    AntiPattern& out) const {

    static const std::pair<std::string, std::regex> registration_patterns[] = {
        {R"(\.AddTool\()", std::regex(R"(\.AddTool\()")},
        {R"(\.Register\()", std::regex(R"(\.Register\()")},
        {R"(\.RegisterHandler\()", std::regex(R"(\.RegisterHandler\()")},
        {R"(\.AddRoute\()", std::regex(R"(\.AddRoute\()")},
        {R"(\.Handle\()", std::regex(R"(\.Handle\()")},
        {R"(\.HandleFunc\()", std::regex(R"(\.HandleFunc\()")},
        {R"(\.Post\()", std::regex(R"(\.Post\()")},
        {R"(\.Get\()", std::regex(R"(\.Get\()")},
        {R"(\.Put\()", std::regex(R"(\.Put\()")},
        {R"(\.Delete\()", std::regex(R"(\.Delete\()")},
        {R"(router\.)", std::regex(R"(router\.)")},
        {R"(mux\.)", std::regex(R"(mux\.)")},
        {R"(app\.Use\()", std::regex(R"(app\.Use\()")},
        {R"(container\.Bind\()", std::regex(R"(container\.Bind\()")},
        {R"(container\.Register)", std::regex(R"(container\.Register)")},
    };

    int total_matches = 0;
    const std::regex* best_re = nullptr;

    for (const auto& [_, re] : registration_patterns) {
        int matches = count_regex_matches(content, re);
        if (matches > total_matches) {
            total_matches = matches;
            best_re = &re;
        }
    }

    if (total_matches >= registration_calls_threshold && best_re) {
        auto [func_name, line_range] = find_containing_function(content, *best_re);

        out.type = AntiPatternType::RegistrationFunction;
        out.description = "Large registration function with " +
                          std::to_string(total_matches) + " sequential registrations";
        out.location = line_range;
        out.severity = determine_anti_pattern_severity(total_matches, registration_calls_threshold);
        out.suggestion = "Consider self-registering pattern using init() functions or a plugin architecture. Function: " + func_name;
        out.metrics.clear();
        out.metrics["registration_calls"] = total_matches;
        return true;
    }

    return false;
}

bool PatternDetector::detect_enum_aggregation(
    std::string_view content, std::string_view file_path,
    AntiPattern& out) const {

    static const std::regex const_block_re(R"(const\s*\()");
    static const std::regex const_single_re(R"(^const\s+\w+)", std::regex::multiline);
    static const std::regex iota_re(R"(\biota\b)");
    static const std::regex enum_member_re(R"(^\s*\w+\s*=\s*(iota|\d+|"[^"]*"))", std::regex::multiline);
    static const std::regex enum_re(R"(enum\s+\w+\s*\{)");
    static const std::regex export_const_re(R"(export\s+const\s+\w+)");

    int const_blocks = count_regex_matches(content, const_block_re);
    int const_singles = count_regex_matches(content, const_single_re);
    int iota_count = count_regex_matches(content, iota_re);
    int enum_members = count_regex_matches(content, enum_member_re);
    int enum_decls = count_regex_matches(content, enum_re);
    int export_consts = count_regex_matches(content, export_const_re);

    int total = const_blocks + const_singles + iota_count + enum_decls + export_consts + enum_members;

    if (total >= enum_values_threshold) {
        out.type = AntiPatternType::EnumAggregation;
        out.description = "File contains " + std::to_string(total) + " enum/const definitions";
        out.location = "file-wide";
        out.severity = determine_anti_pattern_severity(total, enum_values_threshold);
        out.suggestion = "Consider splitting constants by domain/feature, or using code generation";
        out.metrics.clear();
        out.metrics["const_definitions"] = total;
        out.metrics["enum_declarations"] = enum_decls;
        out.metrics["iota_usages"] = iota_count;
        return true;
    }

    return false;
}

bool PatternDetector::detect_god_object(
    std::string_view content, std::string_view file_path,
    AntiPattern& out) const {

    int line_count = count_lines(content);
    if (line_count < god_object_lines_threshold) return false;

    static const std::regex func_re(R"(^func\s+)", std::regex::multiline);
    static const std::regex method_re(R"(^func\s+\([^)]+\)\s+)", std::regex::multiline);
    static const std::regex class_re(R"(^(class|struct|interface|type)\s+\w+)", std::regex::multiline);

    int func_count = count_regex_matches(content, func_re);
    int method_count = count_regex_matches(content, method_re);
    int type_count = count_regex_matches(content, class_re);

    out.type = AntiPatternType::GodObject;
    out.description = "Large file with " + std::to_string(line_count) +
                      " lines, " + std::to_string(func_count) + " functions/methods";
    out.location = "entire file";
    out.severity = AntiPatternSeverity::High;
    out.suggestion = "Consider splitting into smaller, focused modules by responsibility";
    out.metrics.clear();
    out.metrics["line_count"] = line_count;
    out.metrics["function_count"] = func_count;
    out.metrics["method_count"] = method_count;
    out.metrics["type_count"] = type_count;
    return true;
}

std::vector<AntiPattern> PatternDetector::detect_switch_factories(
    std::string_view content, std::string_view file_path) const {

    std::vector<AntiPattern> patterns;

    static const std::regex switch_re(R"(switch\s+[^{]*\{)", std::regex::multiline);
    static const std::regex select_re(R"(select\s*\{)", std::regex::multiline);

    auto process_matches = [&](const std::regex& re, const char* label) {
        auto begin = std::cregex_iterator(content.data(), content.data() + content.size(), re);
        auto end = std::cregex_iterator();
        for (auto it = begin; it != end; ++it) {
            auto match_end = static_cast<size_t>(it->position() + it->length());
            int case_count = count_cases_in_switch(content, match_end);

            if (case_count >= switch_cases_threshold) {
                int line_num = count_newlines(content.substr(0, static_cast<size_t>(it->position()))) + 1;

                AntiPattern ap;
                ap.type = AntiPatternType::SwitchFactory;
                ap.description = std::string("Large ") + label + " statement with " +
                                 std::to_string(case_count) + " cases";
                ap.location = "line " + std::to_string(line_num);
                ap.severity = determine_anti_pattern_severity(case_count, switch_cases_threshold);
                ap.suggestion = "Consider using a map-based dispatch or strategy pattern";
                ap.metrics["case_count"] = case_count;
                patterns.push_back(std::move(ap));
            }
        }
    };

    process_matches(switch_re, "switch");
    process_matches(select_re, "select");

    return patterns;
}

bool PatternDetector::detect_barrel_file(
    std::string_view content, std::string_view file_path,
    AntiPattern& out) const {

    bool is_barrel = str_ends_with(file_path, "index.ts") ||
                     str_ends_with(file_path, "index.js") ||
                     str_ends_with(file_path, "__init__.py") ||
                     str_ends_with(file_path, "mod.rs") ||
                     str_ends_with(file_path, "exports.ts") ||
                     str_ends_with(file_path, "exports.js");
    if (!is_barrel) return false;

    static const std::regex export_re(R"(^export\s+)", std::regex::multiline);
    static const std::regex reexport_re(R"(^export\s+\*?\s*\{?[^}]*\}?\s*from\s+)", std::regex::multiline);
    static const std::regex import_re(R"(^import\s+)", std::regex::multiline);
    static const std::regex py_export_re(R"(^from\s+\.\w+\s+import\s+)", std::regex::multiline);

    int exports = count_regex_matches(content, export_re);
    int reexports = count_regex_matches(content, reexport_re);
    int imports = count_regex_matches(content, import_re);
    int py_exports = count_regex_matches(content, py_export_re);

    int total_export_like = exports + reexports + py_exports;
    int total_lines = count_lines(content);

    if (total_lines > 0) {
        double export_ratio = static_cast<double>(total_export_like) / total_lines;

        if (export_ratio >= barrel_export_ratio_threshold || total_export_like >= 10) {
            out.type = AntiPatternType::BarrelFile;
            char desc[256];
            std::snprintf(desc, sizeof(desc),
                          "Barrel/index file with %d exports (%.0f%% of lines)",
                          total_export_like, export_ratio * 100);
            out.description = desc;
            out.location = "entire file";
            out.severity = determine_anti_pattern_severity(total_export_like, 10);
            out.suggestion = "Consider direct imports instead of barrel files, or split by feature domain";
            out.metrics.clear();
            out.metrics["export_statements"] = total_export_like;
            out.metrics["import_statements"] = imports;
            out.metrics["total_lines"] = total_lines;
            return true;
        }
    }

    return false;
}

bool PatternDetector::detect_config_aggregation(
    std::string_view content, std::string_view file_path,
    AntiPattern& out) const {

    auto lower_path = to_lower(file_path);
    bool is_config = str_contains(lower_path, "config") ||
                     str_contains(lower_path, "settings") ||
                     str_contains(lower_path, "options");
    if (!is_config) return false;

    static const std::regex field_re(R"(^\s+\w+\s+\w+.*`json:)", std::regex::multiline);
    static const std::regex generic_field_re(R"(^\s+\w+\s+\*?\w+[\[\]]*\s*$)", std::regex::multiline);

    int fields = count_regex_matches(content, field_re);
    if (fields == 0) {
        fields = count_regex_matches(content, generic_field_re);
    }

    if (fields >= config_fields_threshold) {
        out.type = AntiPatternType::ConfigAggregation;
        out.description = "Large config file with " + std::to_string(fields) + " fields";
        out.location = "file-wide";
        out.severity = determine_anti_pattern_severity(fields, config_fields_threshold);
        out.suggestion = "Consider splitting config by module/feature with nested structs";
        out.metrics.clear();
        out.metrics["field_count"] = fields;
        return true;
    }

    return false;
}

std::vector<std::string> PatternDetector::get_recommendations(AntiPatternType type) {
    switch (type) {
        case AntiPatternType::RegistrationFunction:
            return {
                "Use init() functions for self-registration in each module",
                "Implement a plugin architecture with auto-discovery",
                "Use code generation to build registration code",
                "Split registrations by feature domain into separate functions",
            };
        case AntiPatternType::EnumAggregation:
            return {
                "Group related constants into separate files by domain",
                "Use go:generate stringer for type-safe enum handling",
                "Consider using typed constants with string methods",
                "Move constants closer to where they are used",
            };
        case AntiPatternType::GodObject:
            return {
                "Extract cohesive functionality into separate packages",
                "Apply Single Responsibility Principle",
                "Use interfaces to define boundaries",
                "Consider domain-driven design for module organization",
            };
        case AntiPatternType::BarrelFile:
            return {
                "Use direct imports instead of barrel re-exports",
                "If barrels are needed, split by feature domain",
                "Consider tree-shaking implications for bundles",
                "Document explicit public API in a README instead",
            };
        case AntiPatternType::SwitchFactory:
            return {
                "Replace switch with map-based dispatch",
                "Use strategy pattern with registered handlers",
                "Consider polymorphism with interfaces",
                "Extract each case into a separate handler function",
            };
        case AntiPatternType::ConfigAggregation:
            return {
                "Split config by subsystem with nested structs",
                "Use separate config files per module",
                "Consider environment-based config loading",
                "Use config composition instead of single struct",
            };
    }
    return {"Review the code structure for potential improvements"};
}

int PatternDetector::count_lines(std::string_view content) {
    if (content.empty()) return 0;
    int count = static_cast<int>(std::count(content.begin(), content.end(), '\n'));
    if (content.back() != '\n') ++count;
    return count;
}

AntiPatternSeverity determine_anti_pattern_severity(int count, int threshold) {
    double ratio = static_cast<double>(count) / threshold;
    if (ratio >= 2.0) return AntiPatternSeverity::High;
    if (ratio >= 1.5) return AntiPatternSeverity::Medium;
    return AntiPatternSeverity::Low;
}

int count_cases_in_switch(std::string_view content, size_t start_idx) {
    if (start_idx >= content.size()) return 0;

    int depth = 1;
    int case_count = 0;
    size_t i = start_idx;

    while (i < content.size() && depth > 0) {
        if (content[i] == '{') {
            ++depth;
        } else if (content[i] == '}') {
            --depth;
        } else if (i + 4 <= content.size() &&
                   content.substr(i, 4) == "case") {
            bool prev_ok = (i == 0 || !is_alnum_or_underscore(content[i - 1]));
            bool next_ok = (i + 4 >= content.size() || !is_alnum_or_underscore(content[i + 4]));
            if (prev_ok && next_ok) ++case_count;
        } else if (i + 7 <= content.size() &&
                   content.substr(i, 7) == "default") {
            bool prev_ok = (i == 0 || !is_alnum_or_underscore(content[i - 1]));
            bool next_ok = (i + 7 >= content.size() || !is_alnum_or_underscore(content[i + 7]));
            if (prev_ok && next_ok) ++case_count;
        }
        ++i;
    }

    return case_count;
}

}  // namespace git
}  // namespace lci
