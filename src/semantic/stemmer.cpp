#include <lci/semantic/stemmer.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace lci {

// -- Porter2 stemmer implementation -------------------------------------------
// Faithful port of the Snowball Porter2 algorithm (same as Go surgebase/porter2).

namespace {

bool is_vowel(char c) {
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

// Returns the position after the first non-vowel after a vowel,
// or the string length if no such position exists.
size_t find_r(const std::string& word, size_t start) {
    bool found_vowel = false;
    for (size_t i = start; i < word.size(); ++i) {
        if (is_vowel(word[i])) {
            found_vowel = true;
        } else if (found_vowel) {
            return i + 1;
        }
    }
    return word.size();
}

bool ends_with(const std::string& s, std::string_view suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool has_vowel(const std::string& word, size_t start, size_t end) {
    for (size_t i = start; i < end && i < word.size(); ++i) {
        if (is_vowel(word[i])) return true;
    }
    return false;
}

// Check if the word ends with a short syllable.
bool ends_with_short_syllable(const std::string& word) {
    size_t len = word.size();
    if (len == 2) {
        return is_vowel(word[0]) && !is_vowel(word[1]);
    }
    if (len >= 3) {
        char c = word[len - 1];
        char b = word[len - 2];
        char a = word[len - 3];
        return !is_vowel(a) && is_vowel(b) && !is_vowel(c) &&
               c != 'w' && c != 'x' && c != 'Y';
    }
    return false;
}

bool is_short_word(const std::string& word, size_t r1) {
    return r1 >= word.size() && ends_with_short_syllable(word);
}

// Step 0: Remove 's, 's, '
void step0(std::string& word) {
    if (ends_with(word, "'s'")) {
        word.erase(word.size() - 3);
    } else if (ends_with(word, "'s")) {
        word.erase(word.size() - 2);
    } else if (ends_with(word, "'")) {
        word.erase(word.size() - 1);
    }
}

// Step 1a
void step1a(std::string& word) {
    if (ends_with(word, "sses")) {
        word.replace(word.size() - 4, 4, "ss");
        return;
    }
    if (ends_with(word, "ied") || ends_with(word, "ies")) {
        if (word.size() > 4) {
            word.replace(word.size() - 3, 3, "i");
        } else {
            word.replace(word.size() - 3, 3, "ie");
        }
        return;
    }
    if (ends_with(word, "us") || ends_with(word, "ss")) {
        return;
    }
    if (ends_with(word, "s") && word.size() > 2) {
        // Delete s if the preceding part contains a vowel not immediately before s.
        bool has_preceding_vowel = false;
        for (size_t i = 0; i + 2 <= word.size(); ++i) {
            if (is_vowel(word[i])) {
                has_preceding_vowel = true;
                break;
            }
        }
        if (has_preceding_vowel) {
            word.pop_back();
        }
    }
}

// Step 1b
void step1b(std::string& word, size_t r1) {
    if (ends_with(word, "eedly")) {
        if (word.size() - 5 >= r1) {
            word.replace(word.size() - 5, 5, "ee");
        }
        return;
    }
    if (ends_with(word, "eed")) {
        if (word.size() - 3 >= r1) {
            word.replace(word.size() - 3, 3, "ee");
        }
        return;
    }

    bool found = false;
    std::string_view suffixes[] = {"ingly", "edly", "ing", "ed"};
    for (auto suf : suffixes) {
        if (ends_with(word, suf)) {
            size_t prefix_len = word.size() - suf.size();
            if (has_vowel(word, 0, prefix_len)) {
                word.erase(prefix_len);
                found = true;
                break;
            }
        }
    }

    if (!found) return;

    // Post-processing after removal.
    if (ends_with(word, "at") || ends_with(word, "bl") ||
        ends_with(word, "iz")) {
        word.push_back('e');
    } else if (word.size() >= 2) {
        char last = word.back();
        char prev = word[word.size() - 2];
        // Double letter ending.
        if (last == prev &&
            (last == 'b' || last == 'd' || last == 'f' || last == 'g' ||
             last == 'm' || last == 'n' || last == 'p' || last == 'r' ||
             last == 't')) {
            word.pop_back();
        } else if (is_short_word(word, r1)) {
            word.push_back('e');
        }
    }
}

// Step 1c: Replace y/Y with i if preceded by non-vowel and word > 2 chars.
void step1c(std::string& word) {
    if (word.size() > 2 && (word.back() == 'y' || word.back() == 'Y')) {
        if (!is_vowel(word[word.size() - 2])) {
            word.back() = 'i';
        }
    }
}

// Step 2
void step2(std::string& word, size_t r1) {
    struct Rule { std::string_view suffix; std::string_view replacement; };
    static const Rule rules[] = {
        {"ational", "ate"},  {"tional", "tion"}, {"enci", "ence"},
        {"anci", "ance"},    {"abli", "able"},   {"entli", "ent"},
        {"izer", "ize"},     {"ization", "ize"}, {"ation", "ate"},
        {"ator", "ate"},     {"alism", "al"},    {"aliti", "al"},
        {"alli", "al"},      {"fulness", "ful"}, {"ousli", "ous"},
        {"ousness", "ous"},  {"iveness", "ive"}, {"iviti", "ive"},
        {"biliti", "ble"},   {"bli", "ble"},     {"fulli", "ful"},
        {"lessli", "less"},
    };

    for (const auto& [suffix, replacement] : rules) {
        if (ends_with(word, suffix)) {
            if (word.size() - suffix.size() >= r1) {
                word.replace(word.size() - suffix.size(), suffix.size(),
                             replacement);
            }
            return;
        }
    }

    // Special handling for "li" ending.
    if (ends_with(word, "li") && word.size() >= 3) {
        char preceding = word[word.size() - 3];
        if (preceding == 'c' || preceding == 'd' || preceding == 'e' ||
            preceding == 'g' || preceding == 'h' || preceding == 'k' ||
            preceding == 'm' || preceding == 'n' || preceding == 'r' ||
            preceding == 't') {
            if (word.size() - 2 >= r1) {
                word.erase(word.size() - 2);
            }
        }
    }
}

// Step 3
void step3(std::string& word, size_t r1, size_t r2) {
    struct Rule { std::string_view suffix; std::string_view replacement; };
    static const Rule rules[] = {
        {"ational", "ate"}, {"tional", "tion"}, {"alize", "al"},
        {"icate", "ic"},    {"iciti", "ic"},     {"ical", "ic"},
        {"ful", ""},        {"ness", ""},
    };

    for (const auto& [suffix, replacement] : rules) {
        if (ends_with(word, suffix)) {
            if (word.size() - suffix.size() >= r1) {
                word.replace(word.size() - suffix.size(), suffix.size(),
                             replacement);
            }
            return;
        }
    }

    // "ative" must be in R2.
    if (ends_with(word, "ative")) {
        if (word.size() - 5 >= r2) {
            word.erase(word.size() - 5);
        }
    }
}

// Step 4
void step4(std::string& word, size_t r2) {
    static const std::string_view suffixes[] = {
        "ement", "ment", "ance", "ence", "able", "ible",
        "ant",   "ent",  "ism",  "ate",  "iti",  "ous",
        "ive",   "ize",  "al",   "er",   "ic",
    };

    for (auto suffix : suffixes) {
        if (ends_with(word, suffix)) {
            if (word.size() - suffix.size() >= r2) {
                word.erase(word.size() - suffix.size());
            }
            return;
        }
    }

    // "ion" preceded by 's' or 't'.
    if (ends_with(word, "ion") && word.size() >= 4) {
        char before = word[word.size() - 4];
        if ((before == 's' || before == 't') &&
            word.size() - 3 >= r2) {
            word.erase(word.size() - 3);
        }
    }
}

// Step 5
void step5(std::string& word, size_t r1, size_t r2) {
    if (word.back() == 'e') {
        if (word.size() - 1 >= r2) {
            word.pop_back();
        } else if (word.size() - 1 >= r1 &&
                   !ends_with_short_syllable(
                       word.substr(0, word.size() - 1))) {
            // Only remove if what remains is not a short syllable ending.
            // We need to check the word without 'e'.
            std::string without_e = word.substr(0, word.size() - 1);
            if (!ends_with_short_syllable(without_e)) {
                word.pop_back();
            }
        }
    } else if (word.back() == 'l' && ends_with(word, "ll") &&
               word.size() - 1 >= r2) {
        word.pop_back();
    }
}

// Special words that should not be stemmed.
struct SpecialForm { std::string_view input; std::string_view output; };
static const SpecialForm kSpecialForms[] = {
    {"skis", "ski"},
    {"skies", "sky"},
    {"dying", "die"},
    {"lying", "lie"},
    {"tying", "tie"},
    {"idly", "idl"},
    {"gently", "gentl"},
    {"ugly", "ugli"},
    {"early", "earli"},
    {"only", "onli"},
    {"singly", "singl"},
};

// Words that are already stems (invariant).
static const std::string_view kInvariantWords[] = {
    "sky", "news", "howe", "atlas", "cosmos", "bias", "andes",
};

// Prefix handling: set initial Y after vowel to uppercase as marker.
std::string preprocess(const std::string& word) {
    std::string result = word;

    // Handle leading apostrophe.
    if (!result.empty() && result[0] == '\'') {
        result.erase(0, 1);
    }

    // Mark Y as consonant-Y when after vowel or at start.
    if (!result.empty() && result[0] == 'y') {
        result[0] = 'Y';
    }
    for (size_t i = 1; i < result.size(); ++i) {
        if (result[i] == 'y' && is_vowel(result[i - 1])) {
            result[i] = 'Y';
        }
    }

    return result;
}

std::string postprocess(const std::string& word) {
    std::string result = word;
    for (auto& c : result) {
        if (c == 'Y') c = 'y';
    }
    return result;
}

}  // anonymous namespace

std::string porter2_stem(std::string_view input) {
    if (input.size() <= 2) return std::string(input);

    // Lowercase the input.
    std::string word(input);
    std::transform(word.begin(), word.end(), word.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });

    // Check special forms.
    for (const auto& [in, out] : kSpecialForms) {
        if (word == in) return std::string(out);
    }

    // Check invariant words.
    for (auto inv : kInvariantWords) {
        if (word == inv) return word;
    }

    // Handle "gener", "commun", "arsen" prefix exceptions.
    size_t r1_start = 0;
    if (word.starts_with("gener") || word.starts_with("arsen")) {
        r1_start = 5;
    } else if (word.starts_with("commun")) {
        r1_start = 6;
    }

    word = preprocess(word);

    // Calculate R1 and R2 regions.
    size_t r1 = find_r(word, r1_start);
    size_t r2 = find_r(word, r1);

    step0(word);
    step1a(word);

    // Exception words after step 1a.
    static const std::string_view step1a_exceptions[] = {
        "inning", "outing", "canning", "herring", "earring",
        "proceed", "exceed", "succeed",
    };
    for (auto exc : step1a_exceptions) {
        if (word == exc) return postprocess(word);
    }

    step1b(word, r1);
    step1c(word);
    step2(word, r1);
    step3(word, r1, r2);
    step4(word, r2);
    step5(word, r1, r2);

    return postprocess(word);
}

// -- Stemmer class ------------------------------------------------------------

Stemmer::Stemmer(bool enabled, std::string_view algorithm,
                 int min_length, std::unordered_set<std::string> exclusions)
    : enabled_(enabled),
      algorithm_(algorithm.empty() ? "porter2" : std::string(algorithm)),
      min_length_(min_length >= 0 ? min_length : 3),
      exclusions_(std::move(exclusions)) {}

Stemmer Stemmer::disabled() {
    return Stemmer(false, "porter2", 3, {});
}

std::string Stemmer::stem(std::string_view word) const {
    if (!enabled_) return std::string(word);

    // Check exclusions (case-insensitive).
    std::string lower(word);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (exclusions_.count(lower)) return std::string(word);

    // Check minimum length.
    if (static_cast<int>(word.size()) < min_length_) return std::string(word);

    if (algorithm_ == "none") return std::string(word);

    return porter2_stem(word);
}

std::vector<std::string> Stemmer::stem_all(
    const std::vector<std::string>& words) const {
    if (!enabled_) return words;

    std::vector<std::string> result;
    result.reserve(words.size());
    for (const auto& w : words) {
        result.push_back(stem(w));
    }
    return result;
}

std::unordered_map<std::string, std::vector<std::string>>
Stemmer::stem_and_group(const std::vector<std::string>& words) const {
    std::unordered_map<std::string, std::vector<std::string>> groups;
    for (const auto& w : words) {
        groups[stem(w)].push_back(w);
    }
    return groups;
}

bool Stemmer::is_excluded(std::string_view word) const {
    std::string lower(word);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return exclusions_.count(lower) > 0;
}

}  // namespace lci
