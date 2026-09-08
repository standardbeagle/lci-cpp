#pragma once

// Whole-word keyword matching for classifier tables. `kw` (lowercase ASCII)
// matches when it equals a WORD of `name`, words being split on
// non-alphanumeric characters and lower->Upper camel boundaries. "ui"
// matches "ui", "app_ui", "UiButton"; never "build" or "guide". The
// substring matching this replaces classified `querySelector` as a database
// call, `guide/` as UI, and `latest.ts` as test (2026-08-30 sweep).

#include <cctype>
#include <string_view>

namespace lci::analysis {

inline bool contains_word(std::string_view name, std::string_view kw) {
    if (kw.empty()) return false;
    size_t i = 0;
    while (i < name.size()) {
        unsigned char c0 = static_cast<unsigned char>(name[i]);
        if (!std::isalnum(c0)) {
            ++i;
            continue;
        }
        size_t wstart = i;
        size_t wend = name.size();
        for (size_t j = i + 1; j < name.size(); ++j) {
            auto c = static_cast<unsigned char>(name[j]);
            auto prev = static_cast<unsigned char>(name[j - 1]);
            if (!std::isalnum(c) ||
                (std::isupper(c) && std::islower(prev))) {
                wend = j;
                break;
            }
        }
        if (wend - wstart == kw.size()) {
            bool eq = true;
            for (size_t k = 0; k < kw.size(); ++k) {
                char a = static_cast<char>(std::tolower(
                    static_cast<unsigned char>(name[wstart + k])));
                if (a != kw[k]) {
                    eq = false;
                    break;
                }
            }
            if (eq) return true;
        }
        i = wend;
    }
    return false;
}

}  // namespace lci::analysis
