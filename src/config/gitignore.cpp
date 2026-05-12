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

namespace {

// Recursive matcher with proper `/` boundary handling. Mirrors the
// implementation in src/indexing/pipeline_scanner.cpp.
//   `?` matches any single non-`/` char
//   `*` matches zero or more non-`/` chars
//   `**` matches zero or more chars across boundaries
bool gitignore_match_at(std::string_view pattern, size_t px,
                        std::string_view text, size_t tx) {
    while (px < pattern.size()) {
        char c = pattern[px];
        if (c == '*') {
            bool double_star =
                (px + 1 < pattern.size() && pattern[px + 1] == '*');
            if (double_star) {
                size_t next_px = px + 2;
                bool slash_anchored = false;
                if (next_px < pattern.size() && pattern[next_px] == '/') {
                    ++next_px;
                    slash_anchored = true;
                }
                for (size_t end = tx; end <= text.size(); ++end) {
                    if (slash_anchored && end != 0 &&
                        !(end <= text.size() && text[end - 1] == '/')) {
                        continue;
                    }
                    if (gitignore_match_at(pattern, next_px, text, end))
                        return true;
                }
                return false;
            }
            size_t next_px = px + 1;
            for (size_t end = tx;; ++end) {
                if (gitignore_match_at(pattern, next_px, text, end))
                    return true;
                if (end >= text.size() || text[end] == '/') break;
            }
            return false;
        }
        if (c == '?') {
            if (tx >= text.size() || text[tx] == '/') return false;
            ++px; ++tx;
            continue;
        }
        if (tx >= text.size() || c != text[tx]) return false;
        ++px; ++tx;
    }
    return tx == text.size();
}

}  // namespace

bool GitignoreParser::match_glob(std::string_view pattern,
                                 std::string_view text) const {
    return gitignore_match_at(pattern, 0, text, 0);
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
