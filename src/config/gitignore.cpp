#include <lci/config/gitignore.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace lci {

bool GitignoreParser::load_gitignore(const std::string& root_path) {
    auto gitignore_path = std::filesystem::path(root_path) / ".gitignore";
    std::ifstream file(gitignore_path);
    if (!file.is_open()) return true;  // Missing .gitignore is not an error

    std::string line;
    while (std::getline(file, line)) {
        // Trim trailing whitespace
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                                 line.back() == '\r'))
            line.pop_back();
        // Trim leading whitespace
        auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        line = line.substr(first);

        if (line.empty() || line[0] == '#') continue;
        add_pattern(line);
    }
    return !file.bad();
}

void GitignoreParser::add_pattern(std::string_view line) {
    if (line.empty() || line[0] == '#') return;
    patterns_.push_back(parse_pattern(line));
}

GitignorePattern GitignoreParser::parse_pattern(std::string_view line) const {
    GitignorePattern pat;
    std::string text(line);

    // Negation
    if (!text.empty() && text[0] == '!') {
        pat.negate = true;
        text = text.substr(1);
    }

    // Directory-only
    if (!text.empty() && text.back() == '/') {
        pat.directory = true;
        text.pop_back();
    }

    // Absolute (leading /)
    if (!text.empty() && text[0] == '/') {
        pat.absolute = true;
        text = text.substr(1);
    }

    pat.pattern = text;
    pat.type = analyze_pattern(text, pat.prefix, pat.suffix);
    return pat;
}

PatternType GitignoreParser::analyze_pattern(
    std::string_view pattern, std::string& prefix_out,
    std::string& suffix_out) const {
    bool has_wildcard = false;
    for (char c : pattern) {
        if (c == '*' || c == '?' || c == '[') { has_wildcard = true; break; }
    }
    if (!has_wildcard) {
        prefix_out = std::string(pattern);
        suffix_out = std::string(pattern);
        return PatternType::Exact;
    }

    // Simple *.ext pattern -> suffix match
    if (pattern.size() > 1 && pattern[0] == '*' &&
        pattern.find('*', 1) == std::string_view::npos &&
        pattern.find('?', 1) == std::string_view::npos &&
        pattern.find('[', 1) == std::string_view::npos) {
        suffix_out = std::string(pattern.substr(1));
        return PatternType::Suffix;
    }

    // Simple name* pattern -> prefix match
    if (pattern.size() > 1 && pattern.back() == '*' &&
        pattern.find('*') == pattern.size() - 1 &&
        pattern.find('?') == std::string_view::npos &&
        pattern.find('[') == std::string_view::npos) {
        prefix_out = std::string(pattern.substr(0, pattern.size() - 1));
        return PatternType::Prefix;
    }

    return PatternType::Wildcard;
}

bool GitignoreParser::should_ignore(std::string_view path,
                                    bool is_dir) const {
    // Normalize to forward slashes
    std::string normalized(path);
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }

    bool ignored = false;
    for (const auto& pat : patterns_) {
        if (matches_pattern(pat, normalized, is_dir)) {
            ignored = !pat.negate;
        }
    }
    return ignored;
}

bool GitignoreParser::matches_pattern(const GitignorePattern& pat,
                                      std::string_view path,
                                      bool is_dir) const {
    // Directory-only patterns match directories and files inside them
    if (pat.directory) {
        if (is_dir) {
            if (fast_match(pat, path)) return true;
        }
        // Check if file is inside a matching directory
        std::string dir_prefix = pat.pattern + "/";
        if (path.find(dir_prefix) != std::string_view::npos) return true;
        if (path.substr(0, dir_prefix.size()) == dir_prefix) return true;
        return fast_match(pat, path);
    }

    if (pat.absolute) {
        return fast_match(pat, path);
    }

    // Relative pattern: match full path or any suffix
    if (fast_match(pat, path)) return true;

    // Try matching against each path suffix
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' && i + 1 < path.size()) {
            if (fast_match(pat, path.substr(i + 1))) return true;
        }
    }
    return false;
}

bool GitignoreParser::fast_match(const GitignorePattern& pat,
                                 std::string_view path) const {
    switch (pat.type) {
        case PatternType::Exact:
            return path == pat.pattern;
        case PatternType::Prefix:
            return path.substr(0, pat.prefix.size()) == pat.prefix;
        case PatternType::Suffix:
            return path.size() >= pat.suffix.size() &&
                   path.substr(path.size() - pat.suffix.size()) == pat.suffix;
        case PatternType::Wildcard:
            return match_glob(pat.pattern, path);
    }
    return path == pat.pattern;
}

bool GitignoreParser::match_glob(std::string_view pattern,
                                 std::string_view text) const {
    size_t px = 0, tx = 0;
    size_t star_px = std::string_view::npos;
    size_t star_tx = 0;

    while (tx < text.size()) {
        if (px < pattern.size() && pattern[px] == '*') {
            if (px + 1 < pattern.size() && pattern[px + 1] == '*') {
                // ** matches everything including /
                px += 2;
                if (px < pattern.size() && pattern[px] == '/') ++px;
                star_px = px;
                star_tx = tx;
                continue;
            }
            // Single * matches everything except /
            star_px = px + 1;
            star_tx = tx;
            ++px;
            continue;
        }

        if (px < pattern.size() && pattern[px] == '?') {
            if (text[tx] != '/') { ++px; ++tx; continue; }
        } else if (px < pattern.size() && pattern[px] == text[tx]) {
            ++px; ++tx; continue;
        }

        // Backtrack to last wildcard
        if (star_px != std::string_view::npos) {
            px = star_px;
            ++star_tx;
            tx = star_tx;
            continue;
        }
        return false;
    }

    // Consume trailing wildcards
    while (px < pattern.size() && pattern[px] == '*') ++px;
    return px == pattern.size();
}

std::vector<std::string> GitignoreParser::get_exclusion_patterns() const {
    std::vector<std::string> result;
    for (const auto& pat : patterns_) {
        if (pat.negate) continue;
        if (pat.directory) {
            if (pat.absolute)
                result.push_back(pat.pattern + "/**");
            else
                result.push_back("**/" + pat.pattern + "/**");
        } else {
            if (pat.absolute)
                result.push_back(pat.pattern);
            else
                result.push_back("**/" + pat.pattern);
        }
    }
    return result;
}

}  // namespace lci
