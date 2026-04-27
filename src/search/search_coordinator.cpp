#include <lci/search/search_engine.h>

#include <algorithm>
#include <cctype>

#include <absl/container/flat_hash_map.h>

namespace lci {

// -- Search-specific pure helper functions ------------------------------------

int search_line_number(std::string_view content, int offset) {
    if (content.empty()) return 1;
    if (offset < 0) offset = 0;
    if (offset >= static_cast<int>(content.size())) {
        offset = static_cast<int>(content.size()) - 1;
    }
    if (offset < 0) return 1;

    int count = 1;
    for (int i = 0; i < offset; ++i) {
        if (content[static_cast<size_t>(i)] == '\n') ++count;
    }
    return count;
}

int search_line_start(std::string_view content, int offset) {
    if (content.empty() || offset <= 0) return 0;
    if (offset > static_cast<int>(content.size())) {
        offset = static_cast<int>(content.size());
    }
    for (int i = offset - 1; i >= 0; --i) {
        if (content[static_cast<size_t>(i)] == '\n') return i + 1;
    }
    return 0;
}

int search_line_end(std::string_view content, int offset) {
    if (content.empty()) return 0;
    if (offset < 0) offset = 0;
    int len = static_cast<int>(content.size());
    if (offset >= len) return len;
    for (int i = offset; i < len; ++i) {
        if (content[static_cast<size_t>(i)] == '\n') return i;
    }
    return len;
}

bool is_word_character(char c) {
    auto u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
           (u >= '0' && u <= '9') || u == '_';
}

bool is_word_boundary(std::string_view content, int pos) {
    if (pos < 0 || pos > static_cast<int>(content.size())) return true;

    bool prev_is_word = (pos > 0) &&
        is_word_character(content[static_cast<size_t>(pos - 1)]);
    bool curr_is_word = (pos < static_cast<int>(content.size())) &&
        is_word_character(content[static_cast<size_t>(pos)]);

    return prev_is_word != curr_is_word;
}

std::vector<int> find_literal_occurrences(std::string_view content,
                                          std::string_view pattern) {
    std::vector<int> positions;
    if (pattern.empty() || content.empty() ||
        pattern.size() > content.size()) {
        return positions;
    }

    size_t offset = 0;
    while (offset + pattern.size() <= content.size()) {
        auto found = content.find(pattern, offset);
        if (found == std::string_view::npos) break;
        positions.push_back(static_cast<int>(found));
        offset = found + 1;
    }
    return positions;
}

std::vector<int> find_literal_occurrences_ci(std::string_view content,
                                             std::string_view pattern) {
    if (pattern.empty() || content.empty()) return {};

    std::string lower_content(content.size(), '\0');
    for (size_t i = 0; i < content.size(); ++i) {
        lower_content[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(content[i])));
    }

    std::string lower_pattern(pattern.size(), '\0');
    for (size_t i = 0; i < pattern.size(); ++i) {
        lower_pattern[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(pattern[i])));
    }

    return find_literal_occurrences(lower_content, lower_pattern);
}

std::vector<int> find_whole_word_occurrences(std::string_view content,
                                             std::string_view pattern) {
    auto positions = find_literal_occurrences(content, pattern);
    std::vector<int> words;
    int pat_len = static_cast<int>(pattern.size());
    for (int pos : positions) {
        if (is_word_boundary(content, pos) &&
            is_word_boundary(content, pos + pat_len)) {
            words.push_back(pos);
        }
    }
    return words;
}

int calculate_pattern_complexity(std::string_view pattern) {
    if (pattern.empty()) return 0;

    int complexity = static_cast<int>(pattern.size());

    for (size_t i = 1; i < pattern.size(); ++i) {
        auto prev = static_cast<unsigned char>(pattern[i - 1]);
        auto curr = static_cast<unsigned char>(pattern[i]);
        if (curr >= 'A' && curr <= 'Z' && prev >= 'a' && prev <= 'z') {
            complexity += 2;
        }
    }

    for (char c : pattern) {
        if (c == '_') ++complexity;
        auto u = static_cast<unsigned char>(c);
        if (u >= '0' && u <= '9') ++complexity;
    }

    return complexity;
}

double calculate_match_quality(std::string_view content,
                               int match_start, int match_end,
                               std::string_view pattern) {
    if (match_end <= match_start || match_start < 0 ||
        match_end > static_cast<int>(content.size())) {
        return 0.0;
    }

    double score = kBaseMatchScore;

    if (is_word_boundary(content, match_start) &&
        is_word_boundary(content, match_end)) {
        score += kWordBoundaryBonus;
    }

    int line_start = search_line_start(content, match_start);
    int trimmed_start = line_start;
    for (int i = line_start; i < static_cast<int>(content.size()); ++i) {
        char c = content[static_cast<size_t>(i)];
        if (c != ' ' && c != '\t') {
            trimmed_start = i;
            break;
        }
    }
    if (match_start == trimmed_start) {
        score += kLineStartBonus;
    }

    auto match_sv = content.substr(
        static_cast<size_t>(match_start),
        static_cast<size_t>(match_end - match_start));
    if (match_sv == pattern) {
        score += kExactCaseBonus;
    }

    return score;
}

int search_binary_line_offset(const std::vector<int>& offsets, int offset) {
    if (offsets.empty()) return 1;

    int lo = 0;
    int hi = static_cast<int>(offsets.size()) - 1;

    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (offsets[static_cast<size_t>(mid)] <= offset) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo + 1;
}

// -- SearchCoordinator --------------------------------------------------------

std::vector<SearchResult> SearchCoordinator::deduplicate(
    std::vector<SearchResult> results) {

    if (results.size() <= 1) return results;

    absl::flat_hash_map<uint64_t, size_t> best;

    for (size_t i = 0; i < results.size(); ++i) {
        uint64_t key = (static_cast<uint64_t>(results[i].file_id) << 32) |
                       static_cast<uint64_t>(
                           static_cast<uint32_t>(results[i].line));
        auto it = best.find(key);
        if (it == best.end()) {
            best[key] = i;
        } else if (results[i].score > results[it->second].score) {
            it->second = i;
        }
    }

    std::vector<SearchResult> deduped;
    deduped.reserve(best.size());
    for (auto& [key, idx] : best) {
        deduped.push_back(std::move(results[idx]));
    }
    return deduped;
}

std::vector<SearchResult> SearchCoordinator::merge(
    std::vector<SearchResult> a,
    std::vector<SearchResult> b) {

    a.reserve(a.size() + b.size());
    for (auto& r : b) {
        a.push_back(std::move(r));
    }
    return deduplicate(std::move(a));
}

void SearchCoordinator::rank(std::vector<SearchResult>& results) {
    std::sort(results.begin(), results.end(),
              [](const SearchResult& a, const SearchResult& b) {
                  if (a.score != b.score) return a.score > b.score;
                  if (a.path != b.path) return a.path < b.path;
                  return a.line < b.line;
              });
}

}  // namespace lci
