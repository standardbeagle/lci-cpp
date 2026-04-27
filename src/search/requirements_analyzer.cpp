#include <lci/search/requirements_analyzer.h>

#include <algorithm>
#include <cctype>

namespace lci {

// -- RequirementsAnalyzer -----------------------------------------------------

RequirementsAnalyzer::RequirementsAnalyzer() = default;

RequirementsAnalyzer::RequirementsAnalyzer(AnalyzerConfig config)
    : config_(std::move(config)) {}

AnalysisResult RequirementsAnalyzer::analyze(
    std::string_view pattern, const SearchOptions& options) const {

    AnalysisResult result;
    result.required_indexes = config_.default_indexes;
    result.confidence = 0.5;
    result.estimated_cost = 100;

    // Always include trigram index for pattern matching.
    add_required(result, IndexType::Trigram,
                 "Pattern matching requires trigram index");

    if (config_.enable_pattern_analysis) {
        analyze_pattern(result, pattern);
    }

    analyze_search_options(result, options);

    if (config_.enable_semantic_analysis) {
        analyze_semantic_requirements(result, pattern);
    }

    if (config_.enable_context_analysis) {
        analyze_context_requirements(result, options);
    }

    calculate_metrics(result, pattern, options);
    generate_optimization_hints(result, pattern, options);

    return result;
}

// -- Pattern analysis ---------------------------------------------------------

void RequirementsAnalyzer::analyze_pattern(AnalysisResult& result,
                                           std::string_view pattern) const {
    // Check for regex patterns (enclosed in /).
    if (pattern.size() > 2 &&
        pattern.front() == '/' && pattern.back() == '/') {
        result.reasoning.emplace_back("Detected regex pattern");
        result.estimated_cost += 200;
        add_optional(result, IndexType::Symbol,
                     "Regex patterns benefit from symbol analysis");
        return;
    }

    int complexity = calculate_pattern_complexity(pattern);
    if (complexity > config_.max_pattern_complexity) {
        result.reasoning.emplace_back("Complex pattern detected");
        result.estimated_cost += 150;
        add_optional(result, IndexType::Symbol,
                     "Complex patterns benefit from symbol analysis");
    }

    if (is_symbol_pattern(pattern)) {
        result.reasoning.emplace_back("Symbol-like pattern detected");
        add_required(result, IndexType::Symbol,
                     "Symbol search requires symbol index");
        result.confidence += 0.2;
    }

    if (is_file_path_pattern(pattern)) {
        result.reasoning.emplace_back("File path pattern detected");
        add_required(result, IndexType::Location,
                     "File path search requires location index");
        result.estimated_cost += 50;
    }

    if (is_content_pattern(pattern)) {
        result.reasoning.emplace_back("Content pattern detected");
        add_optional(result, IndexType::Content,
                     "Content patterns benefit from full-text search");
        result.estimated_cost += 100;
    }
}

void RequirementsAnalyzer::analyze_search_options(
    AnalysisResult& result, const SearchOptions& options) const {

    if (options.declaration_only) {
        result.reasoning.emplace_back("Declaration-only search");
        add_required(result, IndexType::Symbol,
                     "Declaration search requires symbol index");
        add_optional(result, IndexType::Reference,
                     "Declarations may benefit from reference analysis");
        result.confidence += 0.1;
    }

    if (options.usage_only) {
        result.reasoning.emplace_back("Usage-only search");
        add_required(result, IndexType::Reference,
                     "Usage search requires reference index");
        add_optional(result, IndexType::CallGraph,
                     "Usage analysis benefits from call graph");
        result.confidence += 0.1;
    }

    if (options.max_context_lines > 0) {
        result.reasoning.emplace_back("Context lines requested");
        add_required(result, IndexType::Postings,
                     "Context requires postings index");
        add_required(result, IndexType::Content,
                     "Context requires content index");
        result.estimated_cost += static_cast<int64_t>(
            options.max_context_lines) * 20;
    }
}

void RequirementsAnalyzer::analyze_semantic_requirements(
    AnalysisResult& result, std::string_view pattern) const {

    if (has_semantic_annotations(pattern)) {
        result.reasoning.emplace_back("Semantic annotations detected");
        add_required(result, IndexType::Symbol,
                     "Semantic search requires symbol index");
        add_optional(result, IndexType::CallGraph,
                     "Semantic analysis benefits from call graph");
        result.confidence += 0.15;
    }

    if (has_relationship_patterns(pattern)) {
        result.reasoning.emplace_back("Relationship patterns detected");
        add_required(result, IndexType::Reference,
                     "Relationship analysis requires reference index");
        add_optional(result, IndexType::CallGraph,
                     "Complex relationships benefit from call graph");
        result.confidence += 0.1;
    }

    if (has_architectural_patterns(pattern)) {
        result.reasoning.emplace_back("Architectural patterns detected");
        add_required(result, IndexType::Symbol,
                     "Architectural analysis requires symbol index");
        add_required(result, IndexType::CallGraph,
                     "Architectural analysis requires call graph");
        result.confidence += 0.2;
    }
}

void RequirementsAnalyzer::analyze_context_requirements(
    AnalysisResult& result, const SearchOptions& options) const {

    if (options.max_context_lines > 5) {
        result.reasoning.emplace_back("Extensive context requested");
        add_required(result, IndexType::Content,
                     "Extensive context requires content index");
        result.estimated_cost += 100;
    }
}

// -- Metrics ------------------------------------------------------------------

void RequirementsAnalyzer::calculate_metrics(
    AnalysisResult& result,
    std::string_view pattern,
    const SearchOptions& options) const {

    double confidence = 0.3;

    if (pattern.size() > 3) confidence += 0.1;
    if (options.declaration_only || options.usage_only) confidence += 0.2;
    if (result.required_indexes.size() > 2) confidence += 0.1;
    if (confidence > 1.0) confidence = 1.0;

    result.confidence = confidence;

    int64_t cost = 100;
    for (auto idx : result.required_indexes) {
        cost += get_index_cost(idx);
    }
    for (auto idx : result.optional_indexes) {
        cost += get_index_cost(idx) / 2;
    }
    result.estimated_cost = cost;
}

void RequirementsAnalyzer::generate_optimization_hints(
    AnalysisResult& result,
    std::string_view pattern,
    const SearchOptions& options) const {

    if (calculate_pattern_complexity(pattern) >
        config_.max_pattern_complexity) {
        result.optimization_hints.emplace_back(
            "Consider simplifying the search pattern for better performance");
    }

    if (result.required_indexes.size() > 4) {
        result.optimization_hints.emplace_back(
            "Consider using more specific search options to reduce "
            "required indexes");
    }

    if (options.max_context_lines > 10) {
        result.optimization_hints.emplace_back(
            "Large context requests may impact performance");
    }

    if (result.confidence < 0.5) {
        result.optimization_hints.emplace_back(
            "Consider using more specific search terms for better results");
    }
}

// -- Pattern classification helpers -------------------------------------------

int RequirementsAnalyzer::calculate_pattern_complexity(
    std::string_view pattern) const {

    int complexity = static_cast<int>(pattern.size()) / 5;

    for (char c : pattern) {
        if (c == '(' || c == '[' || c == '*') { complexity += 3; break; }
    }

    static constexpr std::string_view operators = "|&^$.+";
    for (char c : pattern) {
        for (char op : operators) {
            if (c == op) ++complexity;
        }
    }

    // Word count approximation.
    bool in_word = false;
    for (char c : pattern) {
        bool is_space = (c == ' ' || c == '\t');
        if (!is_space && !in_word) { ++complexity; in_word = true; }
        else if (is_space) { in_word = false; }
    }

    return complexity;
}

bool RequirementsAnalyzer::is_symbol_pattern(std::string_view pattern) const {
    // Remove regex delimiters if present.
    auto clean = pattern;
    if (clean.size() >= 2 && clean.front() == '/' && clean.back() == '/') {
        clean = clean.substr(1, clean.size() - 2);
    }

    if (clean.empty()) return false;

    // Must match: [a-zA-Z_]([a-zA-Z0-9_]*(\.[a-zA-Z_][a-zA-Z0-9_]*)*)?
    auto is_ident_start = [](char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
    };
    auto is_ident_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };

    size_t i = 0;
    if (!is_ident_start(clean[i])) return false;
    ++i;
    while (i < clean.size() && is_ident_char(clean[i])) ++i;

    while (i < clean.size() && clean[i] == '.') {
        ++i;
        if (i >= clean.size() || !is_ident_start(clean[i])) return false;
        ++i;
        while (i < clean.size() && is_ident_char(clean[i])) ++i;
    }

    return i == clean.size();
}

bool RequirementsAnalyzer::is_file_path_pattern(
    std::string_view pattern) const {
    static constexpr std::string_view extensions[] = {
        ".go", ".js", ".py", ".rs", ".cpp", ".java",
    };
    for (auto c : pattern) {
        if (c == '/' || c == '\\') return true;
    }
    for (auto ext : extensions) {
        if (pattern.size() >= ext.size() &&
            pattern.substr(pattern.size() - ext.size()) == ext) {
            return true;
        }
    }
    return false;
}

bool RequirementsAnalyzer::is_content_pattern(
    std::string_view pattern) const {
    static constexpr std::string_view indicators[] = {
        " ", "\"", "'", "{", "}", "(", ")", ";", ",",
    };
    for (auto ind : indicators) {
        if (pattern.find(ind) != std::string_view::npos) return true;
    }
    return false;
}

bool RequirementsAnalyzer::has_semantic_annotations(
    std::string_view pattern) const {
    static constexpr std::string_view keywords[] = {
        "@lci:", "labels", "category", "depends",
        "critical", "bug", "security",
    };
    for (auto kw : keywords) {
        if (pattern.find(kw) != std::string_view::npos) return true;
    }
    return false;
}

bool RequirementsAnalyzer::has_relationship_patterns(
    std::string_view pattern) const {
    static constexpr std::string_view keywords[] = {
        "calls", "uses", "depends", "implements", "extends", "overrides",
    };
    for (auto kw : keywords) {
        if (pattern.find(kw) != std::string_view::npos) return true;
    }
    return false;
}

bool RequirementsAnalyzer::has_architectural_patterns(
    std::string_view pattern) const {
    static constexpr std::string_view keywords[] = {
        "controller", "service", "repository",
        "model", "view", "handler", "middleware",
    };
    for (auto kw : keywords) {
        if (pattern.find(kw) != std::string_view::npos) return true;
    }
    return false;
}

// -- Static helpers -----------------------------------------------------------

void RequirementsAnalyzer::add_required(AnalysisResult& result,
                                         IndexType type,
                                         std::string_view reason) {
    if (!contains_index(result.required_indexes, type)) {
        result.required_indexes.push_back(type);
        result.reasoning.emplace_back(reason);
    }
}

void RequirementsAnalyzer::add_optional(AnalysisResult& result,
                                         IndexType type,
                                         std::string_view reason) {
    if (!contains_index(result.optional_indexes, type) &&
        !contains_index(result.required_indexes, type)) {
        result.optional_indexes.push_back(type);
        result.reasoning.emplace_back(reason);
    }
}

bool RequirementsAnalyzer::contains_index(
    const std::vector<IndexType>& indexes, IndexType type) {
    return std::find(indexes.begin(), indexes.end(), type) != indexes.end();
}

int64_t RequirementsAnalyzer::get_index_cost(IndexType type) {
    switch (type) {
        case IndexType::Trigram: return 50;
        case IndexType::Symbol: return 100;
        case IndexType::Reference: return 120;
        case IndexType::CallGraph: return 200;
        case IndexType::Postings: return 80;
        case IndexType::Location: return 60;
        case IndexType::Content: return 150;
    }
    return 100;
}

bool RequirementsAnalyzer::should_use_index(const AnalysisResult& result,
                                             IndexType index_type) {
    return contains_index(result.required_indexes, index_type) ||
           contains_index(result.optional_indexes, index_type);
}

int64_t RequirementsAnalyzer::estimated_search_time(
    const AnalysisResult& result) {
    return 5 + (result.estimated_cost / 20);
}

}  // namespace lci
