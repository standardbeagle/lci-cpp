#include <lci/semantic/semantic_scorer.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <unordered_map>

namespace lci {

namespace {

std::string to_lower(std::string_view s) {
    std::string result(s);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return result;
}

// Common abbreviation table matching Go's phrase_matcher.go.
const std::unordered_map<std::string, std::vector<std::string>>& abbreviation_table() {
    static const std::unordered_map<std::string, std::vector<std::string>> table = {
        {"param", {"parameter", "parameters"}},
        {"params", {"parameters", "parameter"}},
        {"arg", {"argument", "arguments"}},
        {"args", {"arguments", "argument"}},
        {"config", {"configuration", "configure"}},
        {"cfg", {"configuration", "config", "configure"}},
        {"msg", {"message", "messages"}},
        {"err", {"error", "errors"}},
        {"req", {"request", "requests"}},
        {"resp", {"response", "responses"}},
        {"res", {"response", "result", "resource"}},
        {"ctx", {"context"}},
        {"conn", {"connection", "connect"}},
        {"db", {"database"}},
        {"auth", {"authentication", "authorize", "authorization"}},
        {"init", {"initialize", "initialization"}},
        {"info", {"information"}},
        {"mgr", {"manager"}},
        {"srv", {"server", "service"}},
        {"svc", {"service"}},
        {"util", {"utility", "utilities"}},
        {"utils", {"utilities", "utility"}},
        {"func", {"function"}},
        {"fn", {"function"}},
        {"var", {"variable"}},
        {"val", {"value"}},
        {"num", {"number"}},
        {"str", {"string"}},
        {"int", {"integer"}},
        {"buf", {"buffer"}},
        {"len", {"length"}},
        {"idx", {"index"}},
        {"ptr", {"pointer"}},
        {"src", {"source"}},
        {"dst", {"destination"}},
        {"dir", {"directory"}},
        {"tmp", {"temporary", "temp"}},
        {"temp", {"temporary"}},
        {"max", {"maximum"}},
        {"min", {"minimum"}},
        {"avg", {"average"}},
        {"cnt", {"count"}},
        {"async", {"asynchronous"}},
        {"sync", {"synchronous", "synchronize"}},
        {"doc", {"document", "documentation"}},
        {"docs", {"documents", "documentation"}},
        {"impl", {"implementation", "implement"}},
        {"exec", {"execute", "execution"}},
        {"cmd", {"command"}},
        {"opt", {"option", "optional"}},
        {"opts", {"options"}},
        {"attr", {"attribute", "attributes"}},
        {"attrs", {"attributes"}},
        {"prop", {"property"}},
        {"props", {"properties"}},
        {"elem", {"element"}},
        {"obj", {"object"}},
        {"ref", {"reference"}},
        {"refs", {"references"}},
        {"prev", {"previous"}},
        {"curr", {"current"}},
        {"desc", {"description", "descending"}},
        {"asc", {"ascending"}},
    };
    return table;
}

bool is_abbreviation_match(std::string_view short_word,
                           std::string_view long_word) {
    const auto& table = abbreviation_table();

    std::string s(short_word);
    std::string l(long_word);

    // Check short -> long.
    auto it = table.find(s);
    if (it != table.end()) {
        for (const auto& exp : it->second) {
            if (exp == l || l.starts_with(exp) || exp.starts_with(l)) {
                return true;
            }
        }
    }

    // Check long -> short (reverse).
    it = table.find(l);
    if (it != table.end()) {
        for (const auto& exp : it->second) {
            if (exp == s || s.starts_with(exp) || exp.starts_with(s)) {
                return true;
            }
        }
    }

    // Prefix match (at least 3 chars).
    if (s.size() >= 3 && l.size() > s.size() && l.starts_with(s)) {
        return true;
    }

    return false;
}

// Simple stem for phrase matching fallback.
std::string simple_stem(std::string_view word) {
    if (word.size() < 4) return std::string(word);

    static const std::string_view suffixes[] = {
        "ing", "tion", "sion", "ment", "ness", "able", "ible",
        "ful", "less", "ous", "ive", "ity", "er", "or", "ly",
        "ed", "es", "s",
    };

    for (auto suf : suffixes) {
        if (word.size() > suf.size() + 2 && word.ends_with(suf)) {
            return std::string(word.substr(0, word.size() - suf.size()));
        }
    }

    return std::string(word);
}

}  // anonymous namespace

// -- ExactMatchDetector -------------------------------------------------------

MatchDetector::DetectResult ExactMatchDetector::detect(
    std::string_view query, std::string_view target_name,
    std::string_view query_lower, std::string_view target_lower,
    const ScoreLayers& config) const {

    if (query_lower == target_lower) {
        return {true, config.exact_weight,
                "Query matches symbol name exactly",
                {{"query", std::string(query)},
                 {"targetName", std::string(target_name)}}};
    }
    return {};
}

// -- SubstringMatchDetector ---------------------------------------------------

MatchDetector::DetectResult SubstringMatchDetector::detect(
    std::string_view query, std::string_view target_name,
    std::string_view query_lower, std::string_view target_lower,
    const ScoreLayers& config) const {

    if (target_lower.find(query_lower) != std::string_view::npos) {
        return {true, config.substring_weight,
                "Symbol name contains query as substring",
                {{"query", std::string(query)},
                 {"targetName", std::string(target_name)}}};
    }
    return {};
}

// -- FuzzyMatchDetector -------------------------------------------------------

FuzzyMatchDetector::FuzzyMatchDetector(const FuzzyMatcher& matcher)
    : matcher_(matcher) {}

MatchDetector::DetectResult FuzzyMatchDetector::detect(
    std::string_view query, std::string_view target_name,
    std::string_view query_lower, std::string_view target_lower,
    const ScoreLayers& config) const {

    double sim = matcher_.similarity(query_lower, target_lower);
    if (sim > config.fuzzy_threshold) {
        double score = config.fuzzy_weight *
            (0.7 + (sim - config.fuzzy_threshold) * 0.1);
        if (score > config.fuzzy_weight) score = config.fuzzy_weight;

        std::ostringstream sim_str;
        sim_str.precision(3);
        sim_str << std::fixed << sim;

        std::ostringstream thresh_str;
        thresh_str.precision(3);
        thresh_str << std::fixed << config.fuzzy_threshold;

        return {true, score,
                std::string("Fuzzy match: '") + std::string(query) +
                    "' resembles '" + std::string(target_name) + "'",
                {{"query", std::string(query)},
                 {"targetName", std::string(target_name)},
                 {"similarity", sim_str.str()},
                 {"threshold", thresh_str.str()}}};
    }
    return {};
}

// -- StemmingMatchDetector ----------------------------------------------------

StemmingMatchDetector::StemmingMatchDetector(const NameSplitter& splitter,
                                             const Stemmer& stemmer)
    : splitter_(splitter), stemmer_(stemmer) {}

MatchDetector::DetectResult StemmingMatchDetector::detect(
    std::string_view query, std::string_view target_name,
    std::string_view query_lower, std::string_view target_lower,
    const ScoreLayers& config) const {

    auto query_words = splitter_.split(query_lower);
    auto target_words = splitter_.split(target_lower);

    if (query_words.empty() || target_words.empty()) return {};

    // Stem query words meeting minimum length.
    std::vector<std::string> query_stemmed;
    query_stemmed.reserve(query_words.size());
    for (const auto& w : query_words) {
        if (static_cast<int>(w.size()) >= config.stem_min_length) {
            query_stemmed.push_back(porter2_stem(w));
        }
    }

    std::vector<std::string> target_stemmed;
    target_stemmed.reserve(target_words.size());
    for (const auto& w : target_words) {
        if (static_cast<int>(w.size()) >= config.stem_min_length) {
            target_stemmed.push_back(porter2_stem(w));
        }
    }

    if (query_stemmed.empty() || target_stemmed.empty()) return {};

    int matched = 0;
    std::vector<std::string> stem_matches;
    for (const auto& qs : query_stemmed) {
        for (const auto& ts : target_stemmed) {
            if (qs == ts) {
                ++matched;
                stem_matches.push_back(qs);
                break;
            }
        }
    }

    if (matched > 0) {
        double ratio = static_cast<double>(matched) /
                       static_cast<double>(query_stemmed.size());
        double score = config.stemming_weight * ratio;

        std::string joined;
        for (size_t i = 0; i < stem_matches.size(); ++i) {
            if (i > 0) joined += ", ";
            joined += stem_matches[i];
        }

        return {true, score,
                "Stemming match: " + joined,
                {{"query", std::string(query)},
                 {"targetName", std::string(target_name)},
                 {"matchedStems", joined},
                 {"matchCount", std::to_string(matched)},
                 {"totalStems", std::to_string(static_cast<int>(query_stemmed.size()))}}};
    }

    return {};
}

// -- NameSplitMatchDetector ---------------------------------------------------

NameSplitMatchDetector::NameSplitMatchDetector(const NameSplitter& splitter)
    : splitter_(splitter) {}

MatchDetector::DetectResult NameSplitMatchDetector::detect(
    std::string_view query, std::string_view target_name,
    std::string_view query_lower, std::string_view target_lower,
    const ScoreLayers& config) const {

    auto query_words = splitter_.split(query_lower);
    auto target_words = splitter_.split(target_lower);

    if (query_words.empty() || target_words.empty()) return {};

    int matched = 0;
    std::vector<std::string> matched_list;
    for (const auto& qw : query_words) {
        for (const auto& tw : target_words) {
            if (qw == tw) {
                ++matched;
                matched_list.push_back(qw);
                break;
            }
        }
    }

    if (matched > 0) {
        double ratio = static_cast<double>(matched) /
                       static_cast<double>(query_words.size());
        double score = config.name_split_weight * ratio;

        auto join = [](const std::vector<std::string>& v) {
            std::string result;
            for (size_t i = 0; i < v.size(); ++i) {
                if (i > 0) result += ", ";
                result += v[i];
            }
            return result;
        };

        return {true, score,
                "Name split match: " + join(matched_list),
                {{"query", std::string(query)},
                 {"targetName", std::string(target_name)},
                 {"queryWords", join(query_words)},
                 {"targetWords", join(target_words)},
                 {"matchedWords", join(matched_list)},
                 {"matchCount", std::to_string(matched)}}};
    }

    return {};
}

// -- AbbreviationMatchDetector ------------------------------------------------

AbbreviationMatchDetector::AbbreviationMatchDetector(const NameSplitter& splitter)
    : splitter_(splitter) {}

MatchDetector::DetectResult AbbreviationMatchDetector::detect(
    std::string_view query, std::string_view target_name,
    std::string_view query_lower, std::string_view target_lower,
    const ScoreLayers& config) const {

    const auto& table = abbreviation_table();

    // Forward expansion: query expands to match target.
    std::string ql(query_lower);
    std::string tl(target_lower);

    std::vector<std::string> forward_matches;
    auto it = table.find(ql);
    if (it != table.end()) {
        for (const auto& exp : it->second) {
            if (tl.find(exp) != std::string::npos) {
                forward_matches.push_back(exp);
            }
        }
    }

    // Reverse expansion: target words expand to match query.
    auto target_words = splitter_.split(target_lower);
    std::vector<std::string> reverse_matches;
    for (const auto& word : target_words) {
        auto wit = table.find(word);
        if (wit == table.end()) continue;
        for (const auto& exp : wit->second) {
            if (ql.find(exp) != std::string::npos && exp != word) {
                reverse_matches.push_back(word + " -> " + exp);
            }
        }
    }

    auto all_count = forward_matches.size() + reverse_matches.size();
    if (all_count > 0) {
        size_t expanded_size = 0;
        if (it != table.end()) expanded_size = it->second.size();
        auto denom = static_cast<double>(expanded_size + target_words.size());
        if (denom == 0.0) denom = 1.0;
        double ratio = static_cast<double>(all_count) / denom;
        double score = config.abbreviation_weight * ratio;

        // Build all matches list.
        std::vector<std::string> all_matches;
        all_matches.reserve(all_count);
        all_matches.insert(all_matches.end(), forward_matches.begin(),
                           forward_matches.end());
        all_matches.insert(all_matches.end(), reverse_matches.begin(),
                           reverse_matches.end());

        auto join = [](const std::vector<std::string>& v) {
            std::string result;
            for (size_t i = 0; i < v.size(); ++i) {
                if (i > 0) result += ", ";
                result += v[i];
            }
            return result;
        };

        std::string justification;
        if (!forward_matches.empty() && !reverse_matches.empty()) {
            justification = "Bidirectional abbreviation match: " + join(all_matches);
        } else if (!forward_matches.empty()) {
            justification = "Abbreviation expansion: '" + std::string(query) +
                            "' -> " + join(forward_matches);
        } else {
            justification = "Reverse abbreviation match: " + join(reverse_matches);
        }

        return {true, score, justification,
                {{"query", std::string(query)},
                 {"targetName", std::string(target_name)},
                 {"matches", join(all_matches)},
                 {"forwardMatches", std::to_string(forward_matches.size())},
                 {"reverseMatches", std::to_string(reverse_matches.size())}}};
    }

    return {};
}

// -- PhraseMatchDetector ------------------------------------------------------

PhraseMatchDetector::PhraseMatchDetector(const NameSplitter& splitter,
                                         const FuzzyMatcher& fuzzer,
                                         const Stemmer& stemmer)
    : splitter_(splitter), fuzzer_(fuzzer), stemmer_(stemmer) {}

MatchDetector::DetectResult PhraseMatchDetector::detect(
    std::string_view query, std::string_view target_name,
    std::string_view query_lower, std::string_view /*target_lower*/,
    const ScoreLayers& config) const {

    // Only for multi-word queries.
    std::string ql(query_lower);
    if (ql.find(' ') == std::string::npos) return {};

    // Split query on whitespace.
    std::vector<std::string> query_words;
    std::istringstream iss(ql);
    std::string word;
    while (iss >> word) {
        if (!word.empty()) query_words.push_back(word);
    }
    if (query_words.empty()) return {};

    // Split target preserving camelCase boundaries.
    auto target_words = splitter_.split(target_name);
    if (target_words.empty()) return {};

    // Match each query word against target words.
    struct WordMatch {
        std::string query_word;
        std::string target_word;
        int target_index{-1};
        double match_score{};
        bool is_exact{};
        bool is_fuzzy{};
        bool is_stem{};
        bool is_abbrev{};
    };

    std::vector<WordMatch> matches(query_words.size());
    std::vector<bool> used(target_words.size(), false);

    // Pass 1: exact matches.
    for (size_t i = 0; i < query_words.size(); ++i) {
        matches[i].query_word = query_words[i];
        for (size_t j = 0; j < target_words.size(); ++j) {
            if (used[j]) continue;
            if (query_words[i] == target_words[j]) {
                matches[i].target_word = target_words[j];
                matches[i].target_index = static_cast<int>(j);
                matches[i].match_score = 1.0;
                matches[i].is_exact = true;
                used[j] = true;
                break;
            }
        }
    }

    // Pass 2: substring matches.
    for (size_t i = 0; i < matches.size(); ++i) {
        if (matches[i].target_index >= 0) continue;
        for (size_t j = 0; j < target_words.size(); ++j) {
            if (used[j]) continue;
            if (target_words[j].find(query_words[i]) != std::string::npos ||
                query_words[i].find(target_words[j]) != std::string::npos) {
                matches[i].target_word = target_words[j];
                matches[i].target_index = static_cast<int>(j);
                matches[i].match_score = 0.95;
                matches[i].is_exact = true;
                used[j] = true;
                break;
            }
        }
    }

    // Pass 3: fuzzy matches.
    for (size_t i = 0; i < matches.size(); ++i) {
        if (matches[i].target_index >= 0) continue;
        double best_score = 0.0;
        int best_idx = -1;
        std::string best_target;
        for (size_t j = 0; j < target_words.size(); ++j) {
            if (used[j]) continue;
            double sim = fuzzer_.similarity(query_words[i], target_words[j]);
            if (sim >= fuzzer_.threshold() && sim > best_score) {
                best_score = sim;
                best_idx = static_cast<int>(j);
                best_target = target_words[j];
            }
        }
        if (best_idx >= 0) {
            matches[i].target_word = best_target;
            matches[i].target_index = best_idx;
            matches[i].match_score = best_score;
            matches[i].is_fuzzy = true;
            used[static_cast<size_t>(best_idx)] = true;
        }
    }

    // Pass 4: abbreviation matches.
    for (size_t i = 0; i < matches.size(); ++i) {
        if (matches[i].target_index >= 0) continue;
        for (size_t j = 0; j < target_words.size(); ++j) {
            if (used[j]) continue;
            if (is_abbreviation_match(query_words[i], target_words[j])) {
                matches[i].target_word = target_words[j];
                matches[i].target_index = static_cast<int>(j);
                matches[i].match_score = 0.85;
                matches[i].is_abbrev = true;
                used[j] = true;
                break;
            }
        }
    }

    // Pass 5: stem matches.
    for (size_t i = 0; i < matches.size(); ++i) {
        if (matches[i].target_index >= 0) continue;
        std::string qw_stem;
        if (stemmer_.is_enabled()) {
            qw_stem = stemmer_.stem(query_words[i]);
        } else {
            qw_stem = simple_stem(query_words[i]);
        }
        for (size_t j = 0; j < target_words.size(); ++j) {
            if (used[j]) continue;
            std::string tw_stem;
            if (stemmer_.is_enabled()) {
                tw_stem = stemmer_.stem(target_words[j]);
            } else {
                tw_stem = simple_stem(target_words[j]);
            }
            if (qw_stem == tw_stem && !qw_stem.empty()) {
                matches[i].target_word = target_words[j];
                matches[i].target_index = static_cast<int>(j);
                matches[i].match_score = 0.80;
                matches[i].is_stem = true;
                used[j] = true;
                break;
            }
        }
    }

    // Calculate result.
    int matched_count = 0;
    double total_word_score = 0.0;
    int fuzzy_count = 0;
    bool all_exact = true;
    std::vector<std::string> matched_words;

    for (const auto& m : matches) {
        if (m.target_index >= 0) {
            ++matched_count;
            matched_words.push_back(m.query_word);
            total_word_score += m.match_score;
            if (m.is_fuzzy) { ++fuzzy_count; all_exact = false; }
            if (m.is_stem || m.is_abbrev) all_exact = false;
        }
    }

    if (matched_count == 0) return {};

    // Base score.
    double avg_word_score = (total_word_score /
        static_cast<double>(query_words.size())) * 0.85;
    bool all_matched = (matched_count == static_cast<int>(query_words.size()));

    // Check word order.
    int last_idx = -1;
    bool in_order = true;
    for (const auto& m : matches) {
        if (m.target_index < 0) continue;
        if (m.target_index <= last_idx) { in_order = false; break; }
        last_idx = m.target_index;
    }

    double score = avg_word_score;
    constexpr double kExactPhraseBonus = 0.05;
    constexpr double kAllWordsBonus = 0.02;
    constexpr double kWordOrderBonus = 0.03;
    constexpr double kFuzzyPenalty = 0.08;

    if (all_matched && in_order) {
        score += kExactPhraseBonus;
    } else if (all_matched) {
        score += kAllWordsBonus;
    }

    if (in_order && matched_count > 1) {
        score += kWordOrderBonus * static_cast<double>(matched_count - 1);
    }
    if (!in_order && matched_count > 1) {
        score -= kWordOrderBonus * static_cast<double>(matched_count);
    }
    if (fuzzy_count > 0) {
        score -= kFuzzyPenalty * static_cast<double>(fuzzy_count) /
                 static_cast<double>(matched_count);
    }

    score = std::clamp(score, 0.0, 1.0);

    // Apply phrase weight.
    double final_score = score * config.phrase_weight;

    bool is_exact_phrase = all_matched && in_order && all_exact;

    auto join = [](const std::vector<std::string>& v) {
        std::string result;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i > 0) result += ", ";
            result += v[i];
        }
        return result;
    };

    std::string justification;
    if (all_matched && in_order) {
        justification = "Exact phrase match: ";
    } else if (all_matched) {
        justification = "All words match (unordered): ";
    } else {
        justification = "Partial phrase match: ";
    }
    justification += join(matched_words);

    return {true, final_score, justification,
            {{"query", std::string(query)},
             {"symbolName", std::string(target_name)},
             {"matchedWords", join(matched_words)},
             {"isExactPhrase", is_exact_phrase ? "true" : "false"}}};
}

// -- SemanticScorer -----------------------------------------------------------

SemanticScorer::SemanticScorer(std::shared_ptr<NameSplitter> splitter,
                               Stemmer stemmer, FuzzyMatcher fuzzer)
    : config_(kDefaultScoreLayers),
      splitter_(std::move(splitter)),
      stemmer_(std::move(stemmer)),
      fuzzer_(std::move(fuzzer)),
      query_cache_(1000) {

    // Build detectors in priority order matching Go.
    detectors_.push_back(std::make_unique<ExactMatchDetector>());
    detectors_.push_back(std::make_unique<SubstringMatchDetector>());
    detectors_.push_back(std::make_unique<PhraseMatchDetector>(
        *splitter_, fuzzer_, stemmer_));
    // Annotation detector skipped (subtask 7.2).
    detectors_.push_back(std::make_unique<FuzzyMatchDetector>(fuzzer_));
    detectors_.push_back(std::make_unique<StemmingMatchDetector>(
        *splitter_, stemmer_));
    detectors_.push_back(std::make_unique<NameSplitMatchDetector>(*splitter_));
    detectors_.push_back(std::make_unique<AbbreviationMatchDetector>(*splitter_));
}

void SemanticScorer::configure(const ScoreLayers& layers) {
    config_ = layers;
}

MatchType SemanticScorer::index_to_match_type(int index) {
    // Matches detector order (annotation slot skipped).
    static const MatchType types[] = {
        MatchType::Exact,
        MatchType::Substring,
        MatchType::Phrase,
        MatchType::Fuzzy,
        MatchType::Stemming,
        MatchType::NameSplit,
        MatchType::Abbreviation,
    };
    if (index >= 0 && index < static_cast<int>(std::size(types))) {
        return types[index];
    }
    return MatchType::None;
}

double SemanticScorer::match_type_to_confidence(MatchType mt) {
    switch (mt) {
        case MatchType::Exact: return 1.0;
        case MatchType::Substring: return 0.95;
        case MatchType::Phrase: return 0.92;
        case MatchType::Annotation: return 0.90;
        case MatchType::Fuzzy: return 0.80;
        case MatchType::Stemming: return 0.70;
        case MatchType::NameSplit: return 0.60;
        case MatchType::Abbreviation: return 0.50;
        case MatchType::None: return 0.0;
    }
    return 0.0;
}

SemanticScore SemanticScorer::score_symbol(std::string_view query,
                                            std::string_view symbol_name) const {
    if (query.empty() || symbol_name.empty()) {
        return {0.0, MatchType::None, 0.0,
                "Empty query or symbol name", {}};
    }

    // Trim whitespace.
    auto trim = [](std::string_view s) -> std::string_view {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
            s.remove_prefix(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
            s.remove_suffix(1);
        return s;
    };

    query = trim(query);
    symbol_name = trim(symbol_name);

    std::string query_lower = to_lower(query);
    std::string symbol_lower = to_lower(symbol_name);

    // Run all detectors, keep best result.
    SemanticScore best{};
    bool found = false;

    for (int i = 0; i < static_cast<int>(detectors_.size()); ++i) {
        auto result = detectors_[static_cast<size_t>(i)]->detect(
            query, symbol_name, query_lower, symbol_lower, config_);

        if (result.matched) {
            MatchType mt = index_to_match_type(i);
            double confidence = match_type_to_confidence(mt);

            if (!found || result.score > best.score) {
                best.score = result.score;
                best.query_match = mt;
                best.confidence = confidence;
                best.justification = std::move(result.justification);
                best.match_details = std::move(result.details);
                found = true;
            }
        }
    }

    if (found) return best;

    return {0.0, MatchType::None, 0.0,
            "No semantic match found", {}};
}

std::vector<ScoredSymbol> SemanticScorer::score_multiple(
    std::string_view query,
    const std::vector<std::string>& symbol_names) const {

    if (symbol_names.empty()) return {};

    std::vector<ScoredSymbol> scored;
    scored.reserve(symbol_names.size());

    for (const auto& name : symbol_names) {
        auto score = score_symbol(query, name);
        if (score.score >= config_.min_score) {
            scored.push_back({name, std::move(score), 0});
        }
    }

    // Sort by score descending, confidence as tiebreaker.
    std::sort(scored.begin(), scored.end(),
              [](const ScoredSymbol& a, const ScoredSymbol& b) {
                  if (a.score.score != b.score.score) {
                      return a.score.score > b.score.score;
                  }
                  return a.score.confidence > b.score.confidence;
              });

    // Limit results.
    int max_results = config_.max_results > 0 ? config_.max_results : 10;
    if (static_cast<int>(scored.size()) > max_results) {
        scored.resize(static_cast<size_t>(max_results));
    }

    // Assign ranks.
    for (int i = 0; i < static_cast<int>(scored.size()); ++i) {
        scored[static_cast<size_t>(i)].rank = i + 1;
    }

    return scored;
}

SemanticSearchResult SemanticScorer::search(
    std::string_view query,
    const std::vector<std::string>& candidates) const {

    auto start = std::chrono::steady_clock::now();

    SemanticSearchResult result;
    result.query = std::string(query);
    result.candidates_considered = static_cast<int>(candidates.size());
    result.symbols = score_multiple(query, candidates);
    result.results_returned = static_cast<int>(result.symbols.size());

    auto end = std::chrono::steady_clock::now();
    result.execution_time_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    return result;
}

void SemanticScorer::clear_cache() {
    query_cache_.clear();
}

}  // namespace lci
