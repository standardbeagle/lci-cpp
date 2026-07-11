#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace lci::text {

inline char ascii_lower_char(char c) {
    unsigned char value = static_cast<unsigned char>(c);
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return c;
}

inline std::string ascii_lower(std::string_view value) {
    std::string result(value);
    for (char& c : result) c = ascii_lower_char(c);
    return result;
}

inline bool ascii_contains_ci(std::string_view haystack,
                              std::string_view needle) {
    if (needle.size() > haystack.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() &&
               ascii_lower_char(haystack[i + j]) ==
                   ascii_lower_char(needle[j])) {
            ++j;
        }
        if (j == needle.size()) return true;
    }
    return false;
}

}  // namespace lci::text
