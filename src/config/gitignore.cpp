#include <lci/config/gitignore.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace lci {

bool GitignoreParser::load_gitignore(const std::string& root_path) {
    return load_dir(root_path, "");
}

// Reads dir/.gitignore, then recurses into subdirectories so nested
// .gitignore files apply to their own subtrees. Patterns from deeper files
// are appended after their parents', and should_ignore's last-match-wins
// scan then gives deeper files precedence — git's rule. Directories already
// ignored by the patterns loaded so far are not descended into: git does
// not read .gitignore files inside ignored directories either (their
// contents are excluded regardless), and skipping them keeps the walk out
// of node_modules-scale trees.
bool GitignoreParser::load_dir(const std::string& dir_path,
                               const std::string& base) {
    namespace fs = std::filesystem;
    bool ok = true;

    auto gitignore_path = fs::path(dir_path) / ".gitignore";
    std::ifstream file(gitignore_path);
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            add_pattern(line, base);
        }
        ok = !file.bad();
    }

    std::error_code ec;
    if (!fs::is_directory(dir_path, ec)) return true;  // nothing to load
    fs::directory_iterator it(dir_path, fs::directory_options::skip_permission_denied, ec);
    if (ec) return false;
    for (const auto& entry : it) {
        if (!entry.is_directory(ec) || entry.is_symlink(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name == ".git") continue;
        std::string child_base =
            base.empty() ? name : base + "/" + name;
        if (should_ignore(child_base, /*is_dir=*/true)) continue;
        if (!load_dir(entry.path().string(), child_base)) ok = false;
    }
    return ok;
}

void GitignoreParser::add_pattern(std::string_view line,
                                  std::string_view base) {
    // Line processing mirrors gitignore(5):
    //  - trailing whitespace is stripped unless backslash-escaped
    //  - leading whitespace is KEPT (it is part of the pattern)
    //  - a leading '#' starts a comment unless backslash-escaped
    //  - backslash escapes the following character
    std::string text(line);
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        // Count preceding backslashes: an odd run escapes this space.
        size_t backslashes = 0;
        for (size_t i = text.size() - 1; i-- > 0 && text[i] == '\\';)
            ++backslashes;
        if (backslashes % 2 == 1) break;
        text.pop_back();
    }

    std::string unescaped;
    unescaped.reserve(text.size());
    bool first_was_escaped = false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            if (unescaped.empty()) first_was_escaped = true;
            unescaped += text[++i];
            continue;
        }
        unescaped += text[i];
    }
    if (unescaped.empty()) return;
    if (!first_was_escaped && unescaped[0] == '#') return;
    patterns_.push_back(parse_pattern(unescaped, base));
}

GitignorePattern GitignoreParser::parse_pattern(std::string_view line,
                                                std::string_view base) const {
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

    // Anchored to the base directory: a leading `/` anchors explicitly, and
    // git anchors ANY pattern with an interior slash too — `doc/*.txt`
    // matches doc/a.txt but not x/doc/a.txt. Only slash-free patterns float.
    if (!text.empty() && text[0] == '/') {
        text = text.substr(1);
        pat.absolute = true;
    } else if (text.find('/') != std::string::npos) {
        pat.absolute = true;
    }

    pat.pattern = text;
    pat.base = std::string(base);
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

// Matches `rel` (path relative to the pattern's base directory): anchored
// patterns must match the whole path; slash-free patterns float to any
// depth, i.e. match any suffix after a component boundary.
bool GitignoreParser::match_rel(const GitignorePattern& pat,
                                std::string_view rel) const {
    if (pat.absolute) return fast_match(pat, rel);
    if (fast_match(pat, rel)) return true;
    for (size_t i = 0; i < rel.size(); ++i) {
        if (rel[i] == '/' && i + 1 < rel.size()) {
            if (fast_match(pat, rel.substr(i + 1))) return true;
        }
    }
    return false;
}

bool GitignoreParser::matches_pattern(const GitignorePattern& pat,
                                      std::string_view path,
                                      bool is_dir) const {
    // A pattern from a nested .gitignore applies only under its directory.
    std::string_view rel = path;
    if (!pat.base.empty()) {
        if (rel.size() <= pat.base.size() ||
            rel.compare(0, pat.base.size(), pat.base) != 0 ||
            rel[pat.base.size()] != '/') {
            return false;
        }
        rel.remove_prefix(pat.base.size() + 1);
    }

    // Literal prefilter (Wildcard only): substring presence is required
    // wherever the pattern would match — full path or any suffix — so one
    // find() replaces the glob matcher and the per-suffix retry loop for
    // the common non-matching file.
    if (!pat.literal.empty() &&
        rel.find(pat.literal) == std::string_view::npos) {
        return false;
    }

    // Directory-only patterns match a directory whose whole path (anchored)
    // or basename-at-any-depth (floating) equals the pattern, plus every
    // file UNDER such a directory. The file case walks ancestor components
    // and matches each through the same anchored/float rule, so wildcard
    // directory patterns (*.egg-info/) match their contents and anchored
    // ones (/build/) refuse src/build/x.c.
    if (pat.directory) {
        if (is_dir && match_rel(pat, rel)) return true;
        for (size_t i = 0; i < rel.size(); ++i) {
            if (rel[i] == '/' && match_rel(pat, rel.substr(0, i))) {
                return true;
            }
        }
        return false;
    }

    return match_rel(pat, rel);
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
        // A pattern from a nested .gitignore is relative to its directory;
        // expressed against the root it becomes anchored there.
        std::string p = pat.pattern;
        if (!pat.base.empty()) p = pat.base + "/" + p;
        const bool anchored = pat.absolute || !pat.base.empty();
        if (pat.directory) {
            result.push_back(anchored ? p + "/**" : "**/" + p + "/**");
        } else {
            result.push_back(anchored ? p : "**/" + p);
        }
    }
    return result;
}

}  // namespace lci
