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

    if (pat.type == PatternType::Wildcard) {
        // Longest run without wildcards or a character class. `[...]`
        // counts as one wildcard position; `**/` can collapse, so boundary
        // slashes are trimmed off the literal.
        std::string best, current;
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (c == '*' || c == '?') {
                if (current.size() > best.size()) best = current;
                current.clear();
            } else if (c == '[') {
                if (current.size() > best.size()) best = current;
                current.clear();
                auto close = text.find(']', i + 1);
                if (close == std::string::npos) break;
                i = close;
            } else {
                current += c;
            }
        }
        if (current.size() > best.size()) best = std::move(current);
        while (!best.empty() && best.front() == '/') best.erase(best.begin());
        while (!best.empty() && best.back() == '/') best.pop_back();
        if (best.size() >= 3) pat.literal = std::move(best);
    }
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
    // Fast path: on POSIX, paths from std::filesystem::relative + generic_string()
    // never contain backslashes, so we can match against the caller's buffer
    // without allocating. Only fall back to a normalized copy if a backslash
    // is actually present (Windows callers passing native separators).
    if (path.find('\\') == std::string_view::npos) {
        bool ignored = false;
        for (const auto& pat : patterns_) {
            if (matches_pattern(pat, path, is_dir)) {
                ignored = !pat.negate;
            }
        }
        return ignored;
    }

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
    // Literal prefilter (Wildcard only): substring presence is required
    // wherever the pattern would match — full path or any suffix — so one
    // find() replaces the glob matcher and the per-suffix retry loop for
    // the common non-matching file.
    if (!pat.literal.empty() &&
        path.find(pat.literal) == std::string_view::npos) {
        return false;
    }
    // Directory-only patterns match directories and files inside them
    if (pat.directory) {
        if (is_dir) {
            if (fast_match(pat, path)) return true;
        }
        // Check if the file lives inside a matching directory. The pattern
        // must cover a WHOLE path component: a plain substring search lets
        // `build/` swallow `prebuild/foo.go` and `rebuild/foo.go`, silently
        // dropping first-party sources from the index. Walk component starts
        // instead — allocation-free, unlike building a `pattern + "/"` key.
        const std::string_view needle = pat.pattern;
        if (!needle.empty()) {
            size_t pos = 0;
            while (pos + needle.size() < path.size()) {
                if (path.compare(pos, needle.size(), needle) == 0 &&
                    path[pos + needle.size()] == '/') {
                    return true;
                }
                auto slash = path.find('/', pos);
                if (slash == std::string_view::npos) break;
                pos = slash + 1;
            }
        }
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
//   `[abc]` / `[a-z]` / `[!abc]` match a single non-`/` char class
//
// The class support is not optional polish: analyze_pattern() already
// classifies any `[` as PatternType::Wildcard, so without it `*.p[yc]`
// reached this matcher and compared `[` as a literal byte — the pattern
// could never match anything.

/// Matches one character class starting at `pattern[px] == '['` against `ch`.
/// Returns false if the class is unterminated (caller treats `[` literally);
/// otherwise sets `matched` and points `px_out` past the closing `]`.
bool match_char_class(std::string_view pattern, size_t px, char ch,
                      bool& matched, size_t& px_out) {
    size_t i = px + 1;
    bool negate = false;
    if (i < pattern.size() && (pattern[i] == '!' || pattern[i] == '^')) {
        negate = true;
        ++i;
    }
    bool found = false;
    bool first = true;
    for (; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == ']' && !first) break;
        first = false;
        // Range `a-z`; a trailing `-` before `]` is a literal.
        if (i + 2 < pattern.size() && pattern[i + 1] == '-' &&
            pattern[i + 2] != ']') {
            if (ch >= c && ch <= pattern[i + 2]) found = true;
            i += 2;
            continue;
        }
        if (c == ch) found = true;
    }
    if (i >= pattern.size()) return false;  // Unterminated: literal '['.
    matched = (found != negate);
    px_out = i + 1;
    return true;
}

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
        if (c == '[') {
            bool cls_matched = false;
            size_t next_px = 0;
            char ch = tx < text.size() ? text[tx] : '\0';
            if (match_char_class(pattern, px, ch, cls_matched, next_px)) {
                if (tx >= text.size() || text[tx] == '/' || !cls_matched)
                    return false;
                px = next_px;
                ++tx;
                continue;
            }
            // Unterminated class: fall through and match '[' literally.
        }
        if (tx >= text.size() || c != text[tx]) return false;
        ++px; ++tx;
    }
    return tx == text.size();
}

}  // namespace

bool glob_match(std::string_view pattern, std::string_view text) {
    return gitignore_match_at(pattern, 0, text, 0);
}

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
