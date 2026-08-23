#include <lci/analysis/finding_suppressions.h>

#include <climits>

namespace lci {
namespace {

constexpr std::string_view kMarker = "lci-";

bool is_rule_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// The rule list runs to the end of the line, stopping at a comment closer
// (`*/`, `-->`, `#>`) so a block-comment directive does not swallow it as a
// rule. Anything that is not a rule character is a separator.
std::vector<std::string> parse_rules(std::string_view rest) {
    std::vector<std::string> rules;
    size_t i = 0;
    while (i < rest.size()) {
        if (rest.substr(i, 2) == "*/" || rest.substr(i, 3) == "-->" ||
            rest.substr(i, 2) == "#>") {
            break;
        }
        if (!is_rule_char(rest[i])) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < rest.size() && is_rule_char(rest[j])) ++j;
        rules.emplace_back(rest.substr(i, j - i));
        i = j;
    }
    return rules;
}

}  // namespace

bool FindingSuppressions::suppresses(int line, std::string_view rule) const {
    for (const auto& e : entries) {
        if (line < e.first_line || line > e.last_line) continue;
        if (e.rules.empty()) return true;
        for (const auto& r : e.rules) {
            if (r == rule) return true;
        }
    }
    return false;
}

FindingSuppressions parse_finding_suppressions(std::string_view source) {
    FindingSuppressions out;
    // Open `lci-disable` blocks awaiting their `lci-enable`, by entry index.
    std::vector<size_t> open_blocks;

    int line = 1;
    size_t pos = 0;
    while (pos < source.size()) {
        size_t eol = source.find('\n', pos);
        if (eol == std::string_view::npos) eol = source.size();
        std::string_view text = source.substr(pos, eol - pos);

        for (size_t at = text.find(kMarker); at != std::string_view::npos;
             at = text.find(kMarker, at + kMarker.size())) {
            std::string_view rest = text.substr(at + kMarker.size());
            auto take = [&](std::string_view word) {
                if (rest.substr(0, word.size()) != word) return false;
                // Whole directive word: `lci-disable-line` must not be read
                // as `lci-disable` + junk.
                if (rest.size() > word.size() && is_rule_char(rest[word.size()]))
                    return false;
                rest.remove_prefix(word.size());
                return true;
            };
            if (take("disable-next-line")) {
                out.entries.push_back({line + 1, line + 1, parse_rules(rest)});
            } else if (take("disable-line")) {
                out.entries.push_back({line, line, parse_rules(rest)});
            } else if (take("disable")) {
                out.entries.push_back({line, INT_MAX, parse_rules(rest)});
                open_blocks.push_back(out.entries.size() - 1);
            } else if (take("enable")) {
                // Closes the innermost open block. A rule list on the enable
                // is accepted and ignored: eslint semantics would re-enable
                // a subset, which needs a second data structure for a case
                // nobody has asked for.
                if (!open_blocks.empty()) {
                    out.entries[open_blocks.back()].last_line = line;
                    open_blocks.pop_back();
                }
            }
            (void)rest;
        }

        pos = eol + 1;
        ++line;
    }
    return out;
}

}  // namespace lci
