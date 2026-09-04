#include <lci/search/search_engine.h>

#include <algorithm>
#include <cctype>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

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

bool line_is_comment_only(std::string_view line) {
    size_t i = 0;
    while (i < line.size() &&
           std::isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
    }
    if (i >= line.size()) return false;
    std::string_view trimmed = line.substr(i);
    while (!trimmed.empty() &&
           std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.remove_suffix(1);
    }
    if (trimmed.empty()) return false;

    // A line is comment-only when it OPENS with a comment marker. The markers
    // are the three line/block openers plus '*', which carries block-comment
    // continuation and close lines (" * text", " */").
    if (trimmed.substr(0, 2) == "//") return true;
    if (trimmed.front() == '#') return true;
    if (trimmed.substr(0, 2) == "/*") return true;
    if (trimmed.front() == '*') return true;

    // Deliberately NOT "the line contains */". That rule deleted real code:
    // `int x = 1; /* note */` and a string literal holding "*/" both matched
    // it, so exclude_comments removed lines the caller had asked for. The
    // residual gap is a line of prose inside a block comment that merely
    // closes it ("trailing prose */"), which is not decidable from the line
    // alone -- it needs cross-line state the search path does not carry. That
    // trade is deliberate and asymmetric: a false negative keeps a comment,
    // which is noise, while a false positive deletes code, which is a wrong
    // answer.
    return false;
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

int search_binary_line_offset(const std::vector<uint32_t>& offsets,
                              int offset) {
    if (offsets.empty()) return 1;
    if (offset < 0) offset = 0;

    int lo = 0;
    int hi = static_cast<int>(offsets.size()) - 1;
    auto target = static_cast<uint32_t>(offset);

    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (offsets[static_cast<size_t>(mid)] <= target) {
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

    // Collect the winning indices and emit in input order. `best` is an absl
    // hash map, so iterating it directly returned the survivors in
    // per-process hash order -- which leaked into rank()'s output whenever
    // two survivors tied on every sort key (Karpathy rule 4).
    std::vector<size_t> keep;
    keep.reserve(best.size());
    for (const auto& [key, idx] : best) keep.push_back(idx);
    std::sort(keep.begin(), keep.end());

    std::vector<SearchResult> deduped;
    deduped.reserve(keep.size());
    for (size_t idx : keep) {
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
    // Total order. score+path+line alone leaves two hits on the same line
    // tied, and std::sort is not stable, so their relative order came from
    // whatever order the caller happened to collect them in -- which for the
    // multi-pattern path is hash order (Karpathy rule 4).
    std::sort(results.begin(), results.end(),
              [](const SearchResult& a, const SearchResult& b) {
                  // Original-pattern rows before synonym-expanded ones, ahead
                  // of score. A synonym is a guess about what the caller
                  // meant; it must not displace a hit on what they wrote.
                  if (a.from_synonym != b.from_synonym) return !a.from_synonym;
                  if (a.score != b.score) return a.score > b.score;
                  if (a.path != b.path) return a.path < b.path;
                  if (a.line != b.line) return a.line < b.line;
                  if (a.column != b.column) return a.column < b.column;
                  return a.match_text < b.match_text;
              });
}

std::vector<std::string> SearchCoordinator::unique_paths(
    std::vector<std::string> paths) {

    if (paths.size() <= 1) return paths;

    absl::flat_hash_set<std::string_view> seen;
    seen.reserve(paths.size());

    // Compact in place: no second buffer, and each survivor is moved rather
    // than copied.
    //
    // Order matters. The seen-set holds views, so a view must be taken from
    // the slot the survivor ENDS UP in, never from the slot it came from:
    // taking it before the move leaves it pointing at a moved-from string, and
    // a later duplicate then goes unrecognized. (For short paths the move
    // copies into the destination's own SSO buffer and abandons the source's,
    // so the stale view reads moved-from bytes rather than the path.)
    //
    // Why a view of paths[keep] stays valid for the rest of the scan: `keep`
    // only ever increases, and the sole write is to paths[keep] immediately
    // before it is incremented, so every subsequent write lands on a strictly
    // greater index. The vector never grows, so no write reallocates and
    // element addresses are stable. The closing resize() only destroys indices
    // at or above the final `keep`, and every registered view sits below it.
    //
    // Costs one extra lookup per survivor (contains, then insert). That is
    // paid once per query over at most a page of rows, and correctness here is
    // not negotiable: reading moved-from state would make the output depend on
    // unspecified values (karpathy rule 4).
    size_t keep = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (seen.contains(std::string_view(paths[i]))) continue;
        if (keep != i) paths[keep] = std::move(paths[i]);
        seen.insert(std::string_view(paths[keep]));
        ++keep;
    }
    paths.resize(keep);
    return paths;
}

}  // namespace lci
