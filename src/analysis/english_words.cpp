#include <lci/analysis/english_words.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <lci/analysis/english_words_data.h>
#include <lci/semantic/stemmer.h>

namespace lci::analysis {

namespace {

// One sorted view over the embedded chunks. The chunks are consecutive
// slices of one sorted file, so a plain concatenation of their lines is
// already globally sorted; the vector holds views into the embedded
// literals (no string copies). Built once, ~95k entries.
const std::vector<std::string_view>& word_index() {
    static const std::vector<std::string_view> idx = [] {
        std::vector<std::string_view> v;
        v.reserve(static_cast<size_t>(kEnglishWordCount));
        for (std::string_view chunk : kEnglishWordChunks) {
            size_t pos = 0;
            while (pos < chunk.size()) {
                size_t nl = chunk.find('\n', pos);
                if (nl == std::string_view::npos) nl = chunk.size();
                if (nl > pos) v.push_back(chunk.substr(pos, nl - pos));
                pos = nl + 1;
            }
        }
        return v;
    }();
    return idx;
}

}  // namespace

bool is_english_word(std::string_view lower_token) {
    const auto& idx = word_index();
    auto it = std::lower_bound(idx.begin(), idx.end(), lower_token);
    return it != idx.end() && *it == lower_token;
}

namespace {

// Word, or a suffix derivation of one via the Porter2 stem (serializer ->
// serial). The stem must stay substantial (>= 5 chars): short stems land on
// unrelated dictionary words and launder genuine typos ("mising" stems to
// the real word "mise").
bool word_or_stemmed_word(std::string_view t) {
    if (is_english_word(t)) return true;
    std::string stem = porter2_stem(t);
    if (stem.size() >= 5 && stem != t) {
        if (is_english_word(stem)) return true;
        if (is_english_word(stem + "e")) return true;
    }
    return false;
}

}  // namespace

bool is_english_like_token(std::string_view lower_token) {
    if (lower_token.size() < 2) return false;
    if (word_or_stemmed_word(lower_token)) return true;

    // Standard derivational prefixes SCOWL does not always pre-expand;
    // the remainder may itself be a suffix derivation (de+serializer).
    static constexpr std::array<std::string_view, 13> kPrefixes = {
        "un",   "re",  "non",  "pre",   "de",    "dis",  "auto",
        "mis",  "sub", "over", "under", "multi", "anti"};
    // The remainder must be a substantial word (>= 4 chars): SCOWL contains
    // fragments like "ing", which would launder "mis"+"ing" — a real typo.
    for (auto p : kPrefixes) {
        if (lower_token.size() >= p.size() + 4 &&
            lower_token.substr(0, p.size()) == p &&
            word_or_stemmed_word(lower_token.substr(p.size())))
            return true;
    }
    return false;
}

}  // namespace lci::analysis
