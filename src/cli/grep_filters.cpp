// Implementation of the grep-compatible filter/transform helpers declared in
// grep_filters.h. Split out of search.cpp (deep-modules campaign): this file
// owns row-level result shaping; search.cpp/grep.cpp own orchestration.

#include "grep_filters.h"

#include <lci/core/mmap.h>
#include <lci/search/search_options.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace lci {
namespace cli {
namespace grep_filters {

std::string to_relative_display_path(const std::string& path) {
    std::error_code cwd_ec;
    auto cwd = std::filesystem::current_path(cwd_ec);
    if (cwd_ec || path.empty()) {
        return path;
    }

    std::error_code rel_ec;
    auto rel = std::filesystem::relative(path, cwd, rel_ec);
    if (!rel_ec && !rel.empty() && rel.string().find("..") == std::string::npos) {
        return rel.string();
    }
    return path;
}

std::string read_line_from_file(const std::string& path, int line_number) {
    if (line_number <= 0 || path.empty()) {
        return {};
    }

    std::ifstream in(path);
    if (!in) {
        return {};
    }

    std::string line;
    for (int current = 1; current <= line_number; ++current) {
        if (!std::getline(in, line)) {
            return {};
        }
    }
    return line;
}

// -- Grep-compatible filter helpers ------------------------------------------
//
// These helpers reshape the raw `results` array returned by the server into
// the four output modes used by the new flags. They share a few invariants:
//
//   - Each entry in `results` is the JSON object emitted by the server's
//     /search endpoint: it has at least `path` and `line` populated.
//   - Operations that need full-file content (--invert-match) read from disk
//     using the absolute `path` field. The server doesn't expose the file
//     buffer over the wire, so reading directly is the simplest correct
//     approach and matches what Go's `displayGrepResults` ends up doing for
//     the few callers that bypass the indexer.

/// Lowercases an ASCII string in place; non-ASCII bytes pass through.
std::string ascii_lower(std::string_view s) {
    std::string out;
    out.resize(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(s[i])));
    }
    return out;
}

/// Returns true if `haystack` contains any of `patterns`. When `patterns` is
/// empty, the function returns false (callers should branch on that case).
bool any_pattern_matches(std::string_view haystack,
                         const std::vector<std::string>& patterns,
                         bool case_insensitive) {
    if (patterns.empty()) return false;
    if (case_insensitive) {
        std::string lower = ascii_lower(haystack);
        for (const auto& p : patterns) {
            std::string lp = ascii_lower(p);
            if (!lp.empty() && lower.find(lp) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    for (const auto& p : patterns) {
        if (!p.empty() && haystack.find(p) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

/// Iterates files in `results` (preserving first-seen order) and produces a
/// new array of synthetic "non-match" rows for invert-match mode.
///
/// A row is shaped like the regular grep result: `path`, `line`, `column=0`,
/// `match` (the raw line text), and a one-line `context` block. This keeps
/// downstream JSON consumers (`--json`) and text formatters happy without
/// special-casing the inverted output.
nlohmann::json invert_match_rows(const nlohmann::json& results,
                                 const std::vector<std::string>& patterns,
                                 bool case_insensitive,
                                 int max_count_per_file) {
    // Preserve file-ordering as it appeared in `results` so output is stable
    // across runs and matches the natural ordering callers expect.
    std::vector<std::string> ordered_paths;
    std::set<std::string> seen;
    for (const auto& r : results) {
        std::string p = r.value("path", "");
        if (p.empty() || seen.contains(p)) continue;
        seen.insert(p);
        ordered_paths.push_back(p);
    }

    nlohmann::json inverted = nlohmann::json::array();
    for (const auto& path : ordered_paths) {
        std::ifstream in(path);
        if (!in) continue;

        std::string line;
        int line_no = 0;
        int kept = 0;
        while (std::getline(in, line)) {
            ++line_no;
            if (any_pattern_matches(line, patterns, case_insensitive)) {
                continue;  // line matched -> excluded from invert output
            }
            nlohmann::json r;
            r["path"] = path;
            r["line"] = line_no;
            r["column"] = 0;
            r["match"] = line;
            nlohmann::json ctx;
            ctx["block_type"] = "lines";
            ctx["start_line"] = line_no;
            ctx["end_line"] = line_no;
            ctx["is_complete"] = true;
            ctx["lines"] = nlohmann::json::array({line});
            ctx["matched_lines"] = nlohmann::json::array({line_no});
            ctx["match_count"] = 1;
            r["context"] = ctx;
            inverted.push_back(std::move(r));
            ++kept;

            if (max_count_per_file > 0 && kept >= max_count_per_file) {
                break;
            }
        }
    }
    return inverted;
}

// Forward decl: defined later in this same anon namespace. Needed because
// apply_word_boundary lands above it in source order.
std::string read_match_line(const nlohmann::json& result,
                            const std::string& path, int line_no);

/// Returns true if byte `c` is a word character (alphanumeric or underscore).
/// Mirrors Go's `\b` semantics in `regexp` package — \w = [A-Za-z0-9_].
bool is_word_byte(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/// Returns true if the match at `[col_start, col_end)` (0-based half-open
/// byte offsets) is surrounded by word boundaries — the byte before the match
/// is non-word (or out of range), and the byte at `col_end` is non-word (or
/// out of range). Byte-level, no regex.
bool match_at_word_boundary(std::string_view line, size_t col_start,
                            size_t col_end) {
    if (col_end > line.size() || col_start >= col_end) return false;
    bool left_ok = (col_start == 0) ||
                   !is_word_byte(static_cast<unsigned char>(line[col_start - 1]));
    bool right_ok = (col_end == line.size()) ||
                    !is_word_byte(static_cast<unsigned char>(line[col_end]));
    return left_ok && right_ok;
}

/// Filters `results` to keep only rows whose match on the matched line is
/// bounded by word boundaries on both sides. Uses 1-based `column` from the
/// result row (server emits 1-based column for matches). Pattern length is
/// taken from the row's `match` field when present, falling back to looking
/// for `pattern` on the matched line.
///
/// Mirrors Go semantics: `findAllMatchesWithOptions` converts a non-regex
/// word-boundary search into a `\b<quoted>\b` regex. We post-filter rows
/// returned by the literal-match engine instead — same effect, no regex.
nlohmann::json apply_word_boundary(nlohmann::json results,
                                   const std::string& pattern,
                                   bool case_insensitive) {
    nlohmann::json out = nlohmann::json::array();
    for (auto& r : results) {
        std::string path = r.value("path", "");
        int line_no = r.value("line", 0);
        int column = r.value("column", 0);
        if (path.empty() || line_no <= 0) {
            // Missing position → can't classify; drop to mirror grep -w
            // strictness (Go's regex `\bfoo\b` would simply not match).
            continue;
        }
        std::string text = read_match_line(r, path, line_no);
        std::string match_str = r.value("match", pattern);
        if (match_str.empty()) match_str = pattern;

        // The server's literal-match engine emits a 0-based byte offset in
        // `column` for match positions on the matched line. (Note: the AST
        // filters in this file treat column as 1-based — that path runs on
        // a DIFFERENT engine output where `column` is post-normalized to
        // 1-based by the formatter. Here we read raw indexer rows pre-
        // formatter, so we use the 0-based byte offset directly.)
        // `column < 0` is treated as "unknown" — fall back to substring scan.
        if (column < 0) {
            // Server didn't record column. Fall back to scanning for the
            // first occurrence on the line.
            size_t pos = std::string::npos;
            if (case_insensitive) {
                std::string lower_line = ascii_lower(text);
                std::string lower_pat = ascii_lower(match_str);
                pos = lower_line.find(lower_pat);
            } else {
                pos = text.find(match_str);
            }
            if (pos == std::string::npos) continue;
            if (!match_at_word_boundary(text, pos, pos + match_str.size())) {
                continue;
            }
        } else {
            size_t start = static_cast<size_t>(column);
            size_t end = start + match_str.size();
            if (!match_at_word_boundary(text, start, end)) continue;
        }
        out.push_back(std::move(r));
    }
    return out;
}

/// Filters `results` to keep only rows whose `path` matches `include_re`
/// (when non-empty) and does NOT match `exclude_re` (when non-empty).
/// Mirrors Go's `displayStandardResults` which calls regexp.MatchString on
/// each result path. Pre-compiled regexes — no per-row compile.
///
/// On invalid regex syntax, emits a warning to stderr and treats the
/// corresponding filter as inactive (matches Go's `regexp.Compile` error
/// path which logs and continues with no filter applied).
nlohmann::json apply_path_filters(nlohmann::json results,
                                  const std::string& exclude_pattern,
                                  const std::string& include_pattern) {
    if (exclude_pattern.empty() && include_pattern.empty()) return results;

    // RE2 instances are compiled ONCE here per call and reused across every
    // result row in the loop below — no recompile per row. RE2 reports compile
    // errors via ok()/error() rather than exceptions (matches Go's
    // regexp.Compile error path which logs+continues with no filter applied).
    RE2::Options opts(RE2::Quiet);
    opts.set_log_errors(false);

    std::unique_ptr<RE2> exclude_re;
    std::unique_ptr<RE2> include_re;
    if (!exclude_pattern.empty()) {
        auto re = std::make_unique<RE2>(exclude_pattern, opts);
        if (re->ok()) {
            exclude_re = std::move(re);
        } else {
            std::cerr << "Warning: invalid --exclude regex '"
                      << exclude_pattern << "': " << re->error()
                      << " (filter ignored)\n";
        }
    }
    if (!include_pattern.empty()) {
        auto re = std::make_unique<RE2>(include_pattern, opts);
        if (re->ok()) {
            include_re = std::move(re);
        } else {
            std::cerr << "Warning: invalid --include regex '"
                      << include_pattern << "': " << re->error()
                      << " (filter ignored)\n";
        }
    }

    if (!exclude_re && !include_re) return results;

    nlohmann::json out = nlohmann::json::array();
    for (auto& r : results) {
        std::string path = r.value("path", "");
        // Include: row must match. Empty path always fails an include filter.
        if (include_re) {
            if (path.empty() || !RE2::PartialMatch(path, *include_re)) continue;
        }
        // Exclude: row must NOT match. Empty path passes (nothing to exclude).
        if (exclude_re && !path.empty() &&
            RE2::PartialMatch(path, *exclude_re)) {
            continue;
        }
        out.push_back(std::move(r));
    }
    return out;
}

/// Strips the `object_id` field from each result row. Used when the user
/// passes `--no-ids` (default is include). Mirrors Go's
/// `searchtypes.PopulateDenseObjectIDs` toggle in cmd/lci/search.go:65 —
/// when `IncludeObjectIDs` is false, object_id stays unset (empty) and the
/// `omitempty` JSON tag drops it from the wire.
nlohmann::json strip_object_ids(nlohmann::json results) {
    for (auto& r : results) {
        if (r.contains("object_id")) r.erase("object_id");
    }
    return results;
}

/// Caps each file's match list at `max_count_per_file`. Pass-through when 0.
nlohmann::json apply_max_count_per_file(nlohmann::json results,
                                        int max_count_per_file) {
    if (max_count_per_file <= 0) return results;
    std::map<std::string, int> per_file;
    nlohmann::json out = nlohmann::json::array();
    for (auto& r : results) {
        std::string p = r.value("path", "");
        int& n = per_file[p];
        if (n >= max_count_per_file) continue;
        ++n;
        out.push_back(std::move(r));
    }
    return out;
}

/// Produces "filename: count" rows for --count. JSON shape uses {path,count}.
nlohmann::json count_per_file_rows(const nlohmann::json& results) {
    // Use the order that paths first appear so output is deterministic and
    // mirrors regular grep's "first hit decides position" behavior.
    std::vector<std::string> ordered;
    std::map<std::string, int> counts;
    for (const auto& r : results) {
        std::string p = r.value("path", "");
        if (p.empty()) continue;
        if (!counts.contains(p)) ordered.push_back(p);
        counts[p] += 1;
    }
    nlohmann::json out = nlohmann::json::array();
    for (const auto& p : ordered) {
        nlohmann::json row;
        row["path"] = p;
        row["count"] = counts[p];
        out.push_back(std::move(row));
    }
    return out;
}

/// Produces unique file paths (in first-seen order) for --files-with-matches.
/// JSON shape uses [{path}, ...] for forward compatibility.
nlohmann::json files_with_matches_rows(const nlohmann::json& results) {
    std::vector<std::string> ordered;
    std::set<std::string> seen;
    for (const auto& r : results) {
        std::string p = r.value("path", "");
        if (p.empty() || seen.contains(p)) continue;
        seen.insert(p);
        ordered.push_back(p);
    }
    nlohmann::json out = nlohmann::json::array();
    for (const auto& p : ordered) {
        nlohmann::json row;
        row["path"] = p;
        out.push_back(std::move(row));
    }
    return out;
}

/// Returns true if the trimmed `line` looks like it starts inside or contains
/// a comment token. Mirrors Go's `Engine.isInComment` logic
/// (internal/search/engine.go:1804): a line is considered "in a comment" if,
/// after trimming leading/trailing whitespace, it starts with `//`, `#`, or
/// `/*`, or anywhere contains `*/`. This is a deliberately cheap heuristic —
/// it does not parse multi-line block comments — but matches Go bit-for-bit
/// so `--exclude-comments` produces the same drop-set across both binaries.
bool line_looks_like_comment(std::string_view line) {
    // Trim leading whitespace.
    size_t i = 0;
    while (i < line.size() &&
           std::isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
    }
    if (i >= line.size()) return false;
    std::string_view trimmed = line.substr(i);
    // Trim trailing whitespace.
    while (!trimmed.empty() &&
           std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.remove_suffix(1);
    }
    if (trimmed.empty()) return false;

    if (trimmed.substr(0, 2) == "//") return true;
    if (trimmed.front() == '#') return true;
    if (trimmed.substr(0, 2) == "/*") return true;
    if (trimmed.find("*/") != std::string_view::npos) return true;
    return false;
}

/// Reads the line text for `(path, line_no)` from the result's embedded
/// `context` block when possible (avoids a disk read), falling back to the
/// file system. The server returns a 1-line window by default, so the context
/// block usually contains the matching line.
std::string read_match_line(const nlohmann::json& result,
                            const std::string& path, int line_no) {
    auto context = result.value("context", nlohmann::json::object());
    int start_line = context.value("start_line", 0);
    auto lines = context.value("lines", nlohmann::json::array());
    if (start_line > 0 && line_no >= start_line) {
        size_t idx = static_cast<size_t>(line_no - start_line);
        if (idx < lines.size()) {
            std::string text = lines[idx].get<std::string>();
            if (!text.empty() && text.back() == '\n') text.pop_back();
            return text;
        }
    }
    std::string text = read_line_from_file(path, line_no);
    if (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

/// Reads `[from_line, to_line]` (1-based, inclusive) from `path`, returning
/// the lines as a vector. Trailing '\n' on each line is stripped to match the
/// formatting expected by JSON consumers and the text formatter.
std::vector<std::string> read_lines_range(const std::string& path,
                                          int from_line, int to_line) {
    std::vector<std::string> out;
    if (from_line <= 0 || to_line < from_line || path.empty()) return out;

    std::ifstream in(path);
    if (!in) return out;

    std::string line;
    int current = 0;
    while (std::getline(in, line)) {
        ++current;
        if (current < from_line) continue;
        if (current > to_line) break;
        if (!line.empty() && line.back() == '\n') line.pop_back();
        out.push_back(std::move(line));
    }
    return out;
}

/// Drops result rows whose `path` looks like a test file. Uses lci's existing
/// `is_test_file()` classifier (basename heuristics: `_test.`, `.test.`,
/// `.spec.`, `test_*`) and additionally drops any path that has a `tests/` or
/// `test/` directory component, so files like `tests/foo.cpp` are filtered
/// out even though their basename doesn't carry a test marker. This matches
/// the task spec: "skip files matching test file patterns (*_test.cpp,
/// test_*.cpp, *Test.cpp, *Tests.cpp, tests/ dir)".
bool path_is_test(std::string_view path) {
    if (lci::is_test_file(path)) return true;

    // Look for `/tests/` or `/test/` directory components (POSIX or Windows
    // separators). Cheap substring scan — paths in results are absolute or
    // relative but always normalized to forward slashes by the server.
    auto contains_dir = [&](std::string_view needle) {
        size_t pos = 0;
        while ((pos = path.find(needle, pos)) != std::string_view::npos) {
            // Match only when surrounded by separators (or at start).
            bool ok_left = (pos == 0) || path[pos - 1] == '/' ||
                           path[pos - 1] == '\\';
            // The needle ends with '/'; nothing to check on the right.
            if (ok_left) return true;
            ++pos;
        }
        return false;
    };
    if (contains_dir("tests/")) return true;
    if (contains_dir("test/")) return true;

    // Catch capitalized basenames Go's tests rely on (FooTest.cpp, FooTests.cpp).
    // The default classifier handles `_test.`, `.test.`, `.spec.`, `test_`
    // but not the trailing `Test`/`Tests` suffix style. Check the basename.
    auto last_slash = path.find_last_of("/\\");
    std::string_view base = (last_slash == std::string_view::npos)
                                ? path
                                : path.substr(last_slash + 1);
    auto last_dot = base.find_last_of('.');
    std::string_view stem = (last_dot == std::string_view::npos)
                                ? base
                                : base.substr(0, last_dot);
    auto ends_with = [](std::string_view s, std::string_view suffix) {
        return s.size() >= suffix.size() &&
               s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (ends_with(stem, "Test") || ends_with(stem, "Tests")) return true;
    return false;
}

/// Filters out result rows whose path matches `path_is_test()`. Stable order.
nlohmann::json apply_exclude_tests(nlohmann::json results) {
    nlohmann::json out = nlohmann::json::array();
    for (auto& r : results) {
        std::string p = r.value("path", "");
        if (!p.empty() && path_is_test(p)) continue;
        out.push_back(std::move(r));
    }
    return out;
}

/// Filters out result rows whose match line looks like it lives inside a
/// comment. Reads the match line via `read_match_line()` (uses the embedded
/// context block when present, falls back to disk). Stable order.
nlohmann::json apply_exclude_comments(nlohmann::json results) {
    nlohmann::json out = nlohmann::json::array();
    for (auto& r : results) {
        std::string path = r.value("path", "");
        int line_no = r.value("line", 0);
        if (path.empty() || line_no <= 0) {
            out.push_back(std::move(r));
            continue;
        }
        std::string text = read_match_line(r, path, line_no);
        if (line_looks_like_comment(text)) continue;
        out.push_back(std::move(r));
    }
    return out;
}

/// Replaces each result's `context` block with one that spans `[line - N,
/// line + N]` read fresh from disk. Used when `--context N` widens the window
/// beyond what the server returned (server defaults to 1 surrounding line).
/// Skipped entirely for invert-match rows — those carry their own pre-built
/// context block over a single synthesized line and rebuilding from disk
/// would be wrong (the row is, by definition, a non-match line).
nlohmann::json widen_context_blocks(nlohmann::json results, int context_lines) {
    if (context_lines <= 0) return results;
    for (auto& r : results) {
        std::string path = r.value("path", "");
        int line_no = r.value("line", 0);
        if (path.empty() || line_no <= 0) continue;

        int from = std::max(1, line_no - context_lines);
        int to = line_no + context_lines;
        auto lines = read_lines_range(path, from, to);
        if (lines.empty()) continue;

        nlohmann::json ctx;
        ctx["block_type"] = "lines";
        ctx["start_line"] = from;
        ctx["end_line"] = from + static_cast<int>(lines.size()) - 1;
        ctx["is_complete"] = true;
        nlohmann::json arr = nlohmann::json::array();
        for (auto& l : lines) arr.push_back(std::move(l));
        ctx["lines"] = std::move(arr);
        nlohmann::json matched = nlohmann::json::array();
        matched.push_back(line_no);
        ctx["matched_lines"] = matched;
        ctx["match_count"] = 1;
        r["context"] = std::move(ctx);
    }
    return results;
}

std::vector<std::string> resolve_scope_paths(
    const std::vector<std::string>& paths, const std::string& root,
    const std::string& cwd) {
    std::vector<std::string> out;
    out.reserve(paths.size());
    const std::string& effective_root = root.empty() ? cwd : root;
    std::filesystem::path root_norm =
        std::filesystem::path(effective_root).lexically_normal();
    std::filesystem::path cwd_path(cwd);
    for (const auto& token : paths) {
        std::filesystem::path p(token);
        std::filesystem::path abs =
            (p.is_absolute() ? p : (cwd_path / p)).lexically_normal();
        std::filesystem::path rel = abs.lexically_relative(root_norm);
        if (!rel.empty() && rel != std::filesystem::path(".") &&
            *rel.begin() != "..") {
            out.push_back(rel.generic_string());
        } else {
            out.push_back(token);
        }
    }
    return out;
}


// Extract the longest literal substring from an ECMAScript regex pattern.
// Used as a trigram seed so the engine narrows files before the local
// regex filter runs. Walks the pattern, skipping escape sequences and
// metacharacters, and tracks the longest contiguous literal run.
/// Every literal run of >=3 chars in `re`, deduplicated, appearance order —
/// the seed set for the regex fast path. The former single longest-run seed
/// was alternation-blind: `(?:panic|unreachable|todo|unimplemented)!` seeded
/// only "unimplemented", silently dropping every other branch's rows
/// (measured 1 of 1203 real sites). Rows containing ANY run are a superset
/// of rows containing the branch that matched; the RE2 row filter decides.
///
/// A run's final char is dropped before a `*`/`?`/`{` quantifier — `abc*`
/// can match "ab", so "abc" would over-narrow; `+` requires its char and
/// keeps it. Runs inside a wholly-optional group can still over-narrow
/// (pre-existing bound, unchanged).
std::vector<std::string> regex_literal_seeds(const std::string& re) {
    std::vector<std::string> seeds;
    std::string cur;
    auto take = [&]() {
        if (cur.size() >= 3 &&
            std::find(seeds.begin(), seeds.end(), cur) == seeds.end()) {
            seeds.push_back(cur);
        }
        cur.clear();
    };
    for (size_t i = 0; i < re.size(); ++i) {
        char c = re[i];
        if (c == '\\' && i + 1 < re.size()) {
            // Escaped char is a literal; consume both.
            char esc = re[i + 1];
            // Skip char-class shorthands (\d, \w, \s, \b, etc.) — those
            // are not literals.
            if (std::isalpha(static_cast<unsigned char>(esc))) {
                take();
            } else {
                cur.push_back(esc);
            }
            ++i;
            continue;
        }
        switch (c) {
            case '*': case '?': case '{':
                // Quantifier can make the preceding char absent from a
                // match — drop it from the run before banking.
                if (!cur.empty()) cur.pop_back();
                take();
                if (c == '{') {
                    while (i + 1 < re.size() && re[i + 1] != '}') ++i;
                }
                break;
            case '(':
                take();
                // Group modifiers are syntax, not literals: skip the
                // "?:" of a non-capturing group (the ':' previously
                // leaked into the first branch's run — ":panic").
                if (i + 2 < re.size() && re[i + 1] == '?' &&
                    re[i + 2] == ':') {
                    i += 2;
                }
                break;
            case '.': case '+':
            case '[': case ']': case ')':
            case '}': case '|':
            case '^': case '$':
                take();
                // Skip whole char class for [...].
                if (c == '[') {
                    while (i + 1 < re.size() && re[i + 1] != ']') ++i;
                }
                break;
            default:
                cur.push_back(c);
                break;
        }
    }
    take();
    return seeds;
}

// Filter result rows by re-matching their content line against the
// user-supplied regex. Drops rows whose context block has no line
// matching the regex; rows that do match get their `line`, `column`,
// and `match` updated to point at the first matching line in the block.
nlohmann::json regex_filter_results(nlohmann::json results,
                                    const RE2& re) {
    // RE2::Match() reports the matched StringPiece into a submatch array;
    // submatch[0] is the whole-match piece. We compute column by subtracting
    // its data() pointer from the line's start — zero copy, no smatch.position()
    // round-trip.
    nlohmann::json out = nlohmann::json::array();
    re2::StringPiece submatches[1];
    // Rows are remapped to whichever context-window line the regex matches,
    // so two seed rows on neighboring lines can land on the SAME output
    // line. Dedup by (path, remapped line): without it `grep -E` printed
    // identical rows back-to-back whenever a context window overlapped the
    // next seed hit.
    std::set<std::pair<std::string, int>> seen;
    for (auto& row : results) {
        auto& ctx = row["context"];
        if (!ctx.is_object() || !ctx.contains("lines") ||
            !ctx["lines"].is_array()) {
            continue;
        }
        int start_line = ctx.value("start_line", 1);
        const int own_line = row.value("line", start_line);
        const int window = static_cast<int>(ctx["lines"].size());

        auto match_at = [&](int idx) -> bool {
            if (idx < 0 || idx >= window) return false;
            const auto& line_json = ctx["lines"][static_cast<size_t>(idx)];
            std::string line = line_json.is_string()
                                   ? line_json.get<std::string>()
                                   : "";
            // Strip trailing newline so $ anchors work as expected.
            if (!line.empty() && line.back() == '\n') line.pop_back();
            re2::StringPiece input(line.data(), line.size());
            if (!re.Match(input, 0, line.size(), RE2::UNANCHORED,
                          submatches, 1)) {
                return false;
            }
            auto position = submatches[0].data() - line.data();
            row["line"] = start_line + idx;
            row["column"] = static_cast<int>(position) + 1;
            row["match"] = std::string(submatches[0].data(),
                                       submatches[0].size());
            return true;
        };

        // The row's OWN matched line takes priority; only when it doesn't
        // match does the rest of the window get a chance (context recovery
        // for regexes whose hit line carried no literal seed).
        bool kept = match_at(own_line - start_line);
        for (int idx = 0; !kept && idx < window; ++idx) {
            if (idx == own_line - start_line) continue;
            kept = match_at(idx);
        }
        if (!kept) continue;
        auto key = std::make_pair(row.value("path", std::string{}),
                                  row.value("line", 0));
        if (seen.contains(key)) continue;
        seen.insert(key);
        out.push_back(std::move(row));
    }
    return out;
}

}  // namespace grep_filters
}  // namespace cli
}  // namespace lci
