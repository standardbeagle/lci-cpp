#include <lci/analysis/ci_vocabulary_analyzer.h>

#include <lci/reference.h>

#include <algorithm>
#include <cctype>
#include <cmath>
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

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
}

}  // namespace

CIVocabularyAnalyzer::CIVocabularyAnalyzer() {
    domain_patterns_["Authentication"] = {
        {"auth", "login", "logout", "user", "password", "token", "session",
         "oauth", "jwt", "credential"},
        1.0, 0.7};
    domain_patterns_["Database"] = {
        {"db", "database", "query", "sql", "table", "schema", "migrate",
         "transaction", "cursor"},
        1.0, 0.8};
    domain_patterns_["HTTP/API"] = {
        {"http", "api", "rest", "endpoint", "request", "response", "handler",
         "route", "server", "client"},
        1.0, 0.8};
    domain_patterns_["Parsing"] = {
        {"parse", "parser", "lexer", "token", "ast", "syntax", "grammar",
         "node"},
        1.0, 0.75};
    domain_patterns_["Testing"] = {
        {"test", "mock", "stub", "assert", "expect", "benchmark", "fixture"},
        1.0, 0.85};
    domain_patterns_["Indexing"] = {
        {"index", "search", "trigram", "symbol", "reference", "cache"},
        1.0, 0.7};
    domain_patterns_["Configuration"] = {
        {"config", "setting", "option", "env", "parameter", "flag"},
        1.0, 0.8};
    domain_patterns_["Error Handling"] = {
        {"error", "err", "exception", "panic", "recover", "fail", "invalid"},
        1.0, 0.65};
    domain_patterns_["Concurrency"] = {
        {"goroutine", "channel", "mutex", "lock", "sync", "async",
         "concurrent", "parallel", "worker"},
        1.0, 0.85};
}

std::pair<std::string, double> CIVocabularyAnalyzer::classify_term_with_strength(
    std::string_view term) const {

    std::string term_lower = to_lower(term);
    std::string best_domain;
    double best_strength = 0.0;

    for (const auto& [domain, pattern] : domain_patterns_) {
        for (const auto& keyword : pattern.keywords) {
            if (term_lower == keyword) {
                if (pattern.exact_weight > best_strength ||
                    (pattern.exact_weight == best_strength &&
                     domain < best_domain)) {
                    best_domain = domain;
                    best_strength = pattern.exact_weight;
                }
            } else if (contains(term_lower, keyword)) {
                double strength = pattern.prefix_weight;
                if (starts_with(term_lower, keyword) ||
                    ends_with(term_lower, keyword)) {
                    strength = pattern.prefix_weight +
                               (pattern.exact_weight - pattern.prefix_weight) *
                                   0.3;
                }
                if (strength > best_strength ||
                    (strength == best_strength && domain < best_domain)) {
                    best_domain = domain;
                    best_strength = strength;
                }
            }
        }
    }

    return {best_domain, best_strength};
}

std::string CIVocabularyAnalyzer::classify_term(std::string_view term) const {
    auto [domain, _] = classify_term_with_strength(term);
    return domain;
}

double CIVocabularyAnalyzer::calculate_domain_confidence(
    double match_strength, int term_count, int total_frequency,
    int total_terms) {

    double confidence = match_strength * 0.4;

    double term_count_factor = 0.0;
    if (term_count > 0) {
        term_count_factor = std::min(
            1.0, std::log10(static_cast<double>(term_count) + 1) /
                     std::log10(11.0));
    }
    confidence += term_count_factor * 0.25;

    double freq_factor = 0.0;
    if (total_frequency > 0) {
        freq_factor = std::min(
            1.0, std::log10(static_cast<double>(total_frequency) + 1) /
                     std::log10(101.0));
    }
    confidence += freq_factor * 0.2;

    double specificity_factor = 0.0;
    if (total_terms > 0) {
        double ratio =
            static_cast<double>(term_count) / static_cast<double>(total_terms);
        specificity_factor = std::min(1.0, ratio * 10.0);
    }
    confidence += specificity_factor * 0.15;

    if (confidence < 0.1) confidence = 0.1;
    if (confidence > 1.0) confidence = 1.0;

    return confidence;
}

std::vector<DomainTerm> CIVocabularyAnalyzer::extract_domain_terms_from_files(
    const std::vector<FileSymbolData>& files) const {

    absl::flat_hash_map<std::string, int> term_frequency;

    for (const auto& file : files) {
        for (const auto* sym : file.symbols) {
            if (!sym->symbol.name.empty()) {
                term_frequency[sym->symbol.name]++;
            }
        }
    }

    struct DomainTermInfo {
        std::vector<std::string> terms;
        int total_freq{};
        double avg_strength{};
        int strength_count{};
    };
    absl::flat_hash_map<std::string, DomainTermInfo> domain_info;

    for (const auto& [term, freq] : term_frequency) {
        auto [domain, strength] = classify_term_with_strength(term);
        if (!domain.empty()) {
            auto& info = domain_info[domain];
            info.terms.push_back(term);
            info.total_freq += freq;
            info.avg_strength += strength;
            info.strength_count++;
        }
    }

    int total_terms = static_cast<int>(term_frequency.size());
    std::vector<DomainTerm> result;

    for (const auto& [domain, info] : domain_info) {
        if (!info.terms.empty()) {
            double avg = info.avg_strength /
                         static_cast<double>(info.strength_count);
            double confidence = calculate_domain_confidence(
                avg, static_cast<int>(info.terms.size()), info.total_freq,
                total_terms);

            DomainTerm dt;
            dt.domain = domain;
            dt.terms = info.terms;
            dt.confidence = confidence;
            dt.count = static_cast<int>(info.terms.size());
            result.push_back(std::move(dt));
        }
    }

    return result;
}

}  // namespace lci
