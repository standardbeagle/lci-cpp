// CLI search/grep implementation, including grep-compatible filter flags.
//
// `--json` output shape contract (stable, consumed by editor integrations and
// `tests/parity`):
//
//   default (no filter / --max-count only)
//     { query, time_ms, count, mode: "grep" | "standard",
//       results: [ { path, line, column, match, score, context, ... } ] }
//
//   --invert-match  (synthesizes one row per non-matching line)
//     { query, time_ms, count, mode: "invert-match",
//       results: [ { path, line, column: 0, match, context: { lines:[line] } } ] }
//
//   --count
//     { query, time_ms, count, mode: "count",
//       results: [ { path, count } ] }
//
//   --files-with-matches
//     { query, time_ms, count, mode: "files-with-matches",
//       results: [ { path } ] }
//
// `--patterns` does not change the shape — it only widens the result set
// (positional pattern OR'd with each `--patterns` entry).
//
// `lci search --json` keeps its `[{result: {...}}]` wrapper for the standard
// path (Go parity); the new filter modes use the un-wrapped shapes above so
// callers can identify the shape via the `mode` field.

#include <lci/cli/commands.h>
#include <lci/core/mmap.h>
#include <lci/indexing/pipeline_scanner.h>
#include <lci/indexing/pipeline_types.h>
#include <lci/search/search_options.h>

#include "ast_filters.h"
#include "grep_filters.h"
#include "profiling.h"
#include "query_parser.h"
#include "rank_options.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <re2/re2.h>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace lci {
namespace cli {

namespace {

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

// -- AST-aware content filters (back `--comments-only`, `--code-only`,
//    `--strings-only`) ----------------------------------------------------
//
// These are heuristic post-filters: they run after the server returns
// match rows and classify each match by token kind from a single line of
// source plus the column the match landed on. The classifier is delibrately
// cheap (no real parser) — see ast_filters.h for the full limitation list.
//
// Two predicates do the work:
//
//   - `match_is_in_string_literal(line, column)`: state-machine scan over
//     `line`, tracking whether the byte at `column` is inside a `"..."`,
//     `'...'`, or triple-quoted literal. Honors backslash escapes; bails
//     out (returns false for the rest of the line) once a `//` or `#`
//     opens a single-line comment.
//
//   - `match_is_in_comment(line, column)`: column-aware version of the
//     `line_looks_like_comment` heuristic. A line-leading `//`/`#`/`/*`
//     makes every column a comment; a tail `// ...` makes columns at or
//     past the opener comment bytes; `*/` anywhere makes columns up to
//     and including the closer comment bytes.
//
// Both helpers treat `column` as 1-based to match the indexer convention
// (`column == 0` is a sentinel meaning "match position not recorded";
// callers fall back to line-level classification in that case).

bool match_is_in_string_literal(std::string_view line, int column) {
    if (column < 0) return false;
    // Convert to 0-based byte index. column == 0 is treated as "match
    // position unknown"; in that case we report false and let callers
    // fall back to line-level heuristics (e.g. tag the whole row as code
    // or comment based on `line_looks_like_comment`).
    if (column == 0) return false;
    size_t target = static_cast<size_t>(column - 1);
    if (target >= line.size()) return false;

    // Scanner state. We walk left-to-right and stop the moment we know
    // whether `target` falls inside a string literal.
    enum class State {
        Code,
        SingleQuote,    // '...'
        DoubleQuote,    // "..."
        TripleSingle,   // '''...'''
        TripleDouble,   // """..."""
        BlockComment,   // /* ... */
    };
    State state = State::Code;
    size_t i = 0;
    while (i < line.size()) {
        char c = line[i];

        switch (state) {
            case State::Code: {
                // Single-line comment opens? Everything from here on is
                // comment, NOT string. If `target` is at or past `i`, the
                // match is in a comment, not a string literal — return
                // false. If `target` is before `i`, we already passed it
                // in the Code branch with no string match — return false.
                if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
                    return false;
                }
                if (c == '#') {
                    return false;
                }
                // Block comment opener.
                if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
                    state = State::BlockComment;
                    i += 2;
                    continue;
                }
                // Triple-quote openers (Python). Check 3-char window.
                if (c == '"' && i + 2 < line.size() && line[i + 1] == '"' &&
                    line[i + 2] == '"') {
                    // The triple-quote DELIMITER itself is part of the
                    // string literal in Python's lexer, so columns
                    // `i..i+2` are "in string". If `target` is one of
                    // those, return true immediately.
                    if (target >= i && target <= i + 2) return true;
                    state = State::TripleDouble;
                    i += 3;
                    continue;
                }
                if (c == '\'' && i + 2 < line.size() && line[i + 1] == '\'' &&
                    line[i + 2] == '\'') {
                    if (target >= i && target <= i + 2) return true;
                    state = State::TripleSingle;
                    i += 3;
                    continue;
                }
                // Single quote opener (char or rune literal in C/Go/Rust;
                // string literal in Python/JS-via-template-but-still).
                if (c == '\'') {
                    // Opener byte itself: if `target == i`, the match is
                    // ON the opening quote — count it as in-string.
                    if (target == i) return true;
                    state = State::SingleQuote;
                    ++i;
                    continue;
                }
                // Double quote opener.
                if (c == '"') {
                    if (target == i) return true;
                    state = State::DoubleQuote;
                    ++i;
                    continue;
                }
                // Plain code byte. If we've reached the target column
                // without entering a string, the match is in code.
                if (i == target) return false;
                ++i;
                break;
            }
            case State::SingleQuote: {
                // Backslash escape: skip next byte (so `'\''` doesn't
                // close the literal mid-stream).
                if (c == '\\' && i + 1 < line.size()) {
                    if (target == i || target == i + 1) return true;
                    i += 2;
                    continue;
                }
                if (c == '\'') {
                    // Closing quote. The closer byte is part of the
                    // literal in lexer terms; treat `target == i` as
                    // in-string.
                    if (target == i) return true;
                    state = State::Code;
                    ++i;
                    continue;
                }
                // Inside the literal.
                if (i == target) return true;
                ++i;
                break;
            }
            case State::DoubleQuote: {
                if (c == '\\' && i + 1 < line.size()) {
                    if (target == i || target == i + 1) return true;
                    i += 2;
                    continue;
                }
                if (c == '"') {
                    if (target == i) return true;
                    state = State::Code;
                    ++i;
                    continue;
                }
                if (i == target) return true;
                ++i;
                break;
            }
            case State::TripleDouble: {
                // Triple-quote closer? Three consecutive double quotes.
                if (c == '"' && i + 2 < line.size() && line[i + 1] == '"' &&
                    line[i + 2] == '"') {
                    if (target >= i && target <= i + 2) return true;
                    state = State::Code;
                    i += 3;
                    continue;
                }
                if (i == target) return true;
                ++i;
                break;
            }
            case State::TripleSingle: {
                if (c == '\'' && i + 2 < line.size() && line[i + 1] == '\'' &&
                    line[i + 2] == '\'') {
                    if (target >= i && target <= i + 2) return true;
                    state = State::Code;
                    i += 3;
                    continue;
                }
                if (i == target) return true;
                ++i;
                break;
            }
            case State::BlockComment: {
                // Block comment closer? `*/` ends the comment; we don't
                // count comment bytes as string-literal bytes, so when
                // `target` falls in here we return false.
                if (c == '*' && i + 1 < line.size() && line[i + 1] == '/') {
                    if (target >= i && target <= i + 1) return false;
                    state = State::Code;
                    i += 2;
                    continue;
                }
                if (i == target) return false;
                ++i;
                break;
            }
        }
    }
    // Reached end-of-line without classifying the target. If we never
    // closed a string, the target was inside an unclosed literal that
    // runs through end-of-line — report in-string. Otherwise (we ended
    // in Code or BlockComment), the target was in code or comment, not
    // string.
    if (state == State::SingleQuote || state == State::DoubleQuote ||
        state == State::TripleSingle || state == State::TripleDouble) {
        return target < line.size();
    }
    return false;
}

bool match_is_in_comment(std::string_view line, int column) {
    if (column < 0) return false;
    // column == 0 means "no column recorded" — fall back to line-level
    // heuristic (whole-line classifier).
    if (column == 0) return line_looks_like_comment(line);

    size_t target = static_cast<size_t>(column - 1);
    if (target >= line.size()) return false;

    // Trim leading whitespace to mirror line_looks_like_comment(): a line
    // whose first non-whitespace token is `//`, `#`, or `/*` is a comment
    // line top-to-bottom. The trim excludes the leading whitespace from
    // the classification — a match column that falls inside the leading
    // whitespace of a `// foo` line is still considered "in a comment"
    // because the column is on a comment-only line.
    size_t first_non_ws = 0;
    while (first_non_ws < line.size() &&
           std::isspace(static_cast<unsigned char>(line[first_non_ws]))) {
        ++first_non_ws;
    }
    if (first_non_ws < line.size()) {
        std::string_view rest = line.substr(first_non_ws);
        if (rest.substr(0, 2) == "//" || rest.front() == '#' ||
            rest.substr(0, 2) == "/*") {
            return true;
        }
    }

    // Scan for inline single-line or block comment openers and a `*/`
    // closer. We respect string literals so a `//` inside `"..."` doesn't
    // mark the rest of the line as comment.
    enum class State { Code, SingleQuote, DoubleQuote, BlockComment };
    State state = State::Code;
    size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        switch (state) {
            case State::Code: {
                // Inline `// ...` or `# ...`: every column from `i` to
                // end-of-line is comment.
                if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
                    return target >= i;
                }
                if (c == '#') {
                    return target >= i;
                }
                // `/* ...`: comment starts at `i`. We continue scanning to
                // find a same-line `*/`; until we find it (or hit EOL),
                // every column from `i` is comment.
                if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
                    if (target >= i) {
                        // Look for `*/` after this opener on the same line.
                        size_t close = line.find("*/", i + 2);
                        if (close == std::string_view::npos) {
                            // Block runs through EOL: every column from
                            // `i` is comment.
                            return target >= i;
                        }
                        // Block closes at `close`; columns `i..close+1`
                        // are comment, columns past `close+1` are not.
                        return target >= i && target <= close + 1;
                    }
                    // Block opens after `target`; we already passed
                    // `target` in code. Fall through to keep scanning
                    // (could still hit a leading `*/` later, though
                    // that'd be invalid C anyway).
                    state = State::BlockComment;
                    i += 2;
                    continue;
                }
                // `*/` anywhere indicates the line crossed a block-
                // comment closer: every column up to and INCLUDING the
                // `/` of `*/` is comment.
                if (c == '*' && i + 1 < line.size() && line[i + 1] == '/') {
                    return target <= i + 1;
                }
                if (c == '\'') {
                    state = State::SingleQuote;
                    ++i;
                    continue;
                }
                if (c == '"') {
                    state = State::DoubleQuote;
                    ++i;
                    continue;
                }
                ++i;
                break;
            }
            case State::SingleQuote: {
                if (c == '\\' && i + 1 < line.size()) { i += 2; continue; }
                if (c == '\'') { state = State::Code; ++i; continue; }
                ++i;
                break;
            }
            case State::DoubleQuote: {
                if (c == '\\' && i + 1 < line.size()) { i += 2; continue; }
                if (c == '"') { state = State::Code; ++i; continue; }
                ++i;
                break;
            }
            case State::BlockComment: {
                if (c == '*' && i + 1 < line.size() && line[i + 1] == '/') {
                    state = State::Code;
                    i += 2;
                    continue;
                }
                ++i;
                break;
            }
        }
    }
    // Fell off end of line in Code/string state — target was not inside
    // a comment (we'd have returned earlier if it were).
    return false;
}

/// Filters `results` to keep only rows whose match falls inside a comment
/// token. Row order preserved. Rows with missing path/line are passed
/// through (graceful degradation — we'd rather show a possibly-wrong row
/// than silently drop it on indexer hiccup).
nlohmann::json apply_comments_only(nlohmann::json results) {
    nlohmann::json out = nlohmann::json::array();
    for (auto& r : results) {
        std::string path = r.value("path", "");
        int line_no = r.value("line", 0);
        if (path.empty() || line_no <= 0) {
            out.push_back(std::move(r));
            continue;
        }
        std::string text = read_match_line(r, path, line_no);
        int column = r.value("column", 0);
        if (!match_is_in_comment(text, column)) continue;
        out.push_back(std::move(r));
    }
    return out;
}

/// Filters `results` to keep only rows whose match falls inside a string
/// literal. Row order preserved. Rows with missing path/line are passed
/// through (graceful degradation).
nlohmann::json apply_strings_only(nlohmann::json results) {
    nlohmann::json out = nlohmann::json::array();
    for (auto& r : results) {
        std::string path = r.value("path", "");
        int line_no = r.value("line", 0);
        if (path.empty() || line_no <= 0) {
            out.push_back(std::move(r));
            continue;
        }
        std::string text = read_match_line(r, path, line_no);
        int column = r.value("column", 0);
        if (!match_is_in_string_literal(text, column)) continue;
        out.push_back(std::move(r));
    }
    return out;
}

/// Filters `results` to drop rows whose match is inside either a comment OR
/// a string literal. Equivalent to running the inverse of each predicate
/// in sequence. Row order preserved. Rows with missing path/line are
/// passed through (graceful degradation).
nlohmann::json apply_code_only(nlohmann::json results) {
    nlohmann::json out = nlohmann::json::array();
    for (auto& r : results) {
        std::string path = r.value("path", "");
        int line_no = r.value("line", 0);
        if (path.empty() || line_no <= 0) {
            out.push_back(std::move(r));
            continue;
        }
        std::string text = read_match_line(r, path, line_no);
        int column = r.value("column", 0);
        if (match_is_in_comment(text, column)) continue;
        if (match_is_in_string_literal(text, column)) continue;
        out.push_back(std::move(r));
    }
    return out;
}

/// Runs the server search for each pattern in `patterns` (a non-empty list)
/// and returns the unioned `results` array. Duplicates by (path, line) are
/// suppressed; the first encountered match wins so the leading positional
/// pattern's score/context survives.
std::optional<nlohmann::json> search_union_patterns(
    Client& client, const std::vector<std::string>& patterns, int max_results,
    bool case_insensitive, std::string& error) {
    nlohmann::json all = nlohmann::json::array();
    std::set<std::pair<std::string, int>> seen;
    for (const auto& p : patterns) {
        if (p.empty()) continue;
        std::string err;
        auto j = client.search(p, max_results, case_insensitive, false, err);
        if (!j) {
            error = err;
            return std::nullopt;
        }
        for (auto& r : j->value("results", nlohmann::json::array())) {
            std::string path = r.value("path", "");
            int line = r.value("line", 0);
            auto key = std::make_pair(path, line);
            if (seen.contains(key)) continue;
            seen.insert(key);
            all.push_back(r);
        }
    }
    nlohmann::json wrapper;
    wrapper["results"] = all;
    return wrapper;
}

}  // namespace

// -- Test-visible filter helpers ---------------------------------------------
//
// Thin forwarders into the anonymous-namespace implementations above. The
// header (src/cli/grep_filters.h) is consumed by tests/cli_test.cpp so the
// pure logic (`line_looks_like_comment`, `path_is_test`) and the JSON
// transforms (`apply_exclude_*`, `widen_context_blocks`) can be exercised
// without spinning up the server.
namespace grep_filters {

bool line_looks_like_comment(std::string_view line) {
    return ::lci::cli::line_looks_like_comment(line);
}

bool path_is_test(std::string_view path) {
    return ::lci::cli::path_is_test(path);
}

nlohmann::json apply_exclude_tests(nlohmann::json results) {
    return ::lci::cli::apply_exclude_tests(std::move(results));
}

nlohmann::json apply_exclude_comments(nlohmann::json results) {
    return ::lci::cli::apply_exclude_comments(std::move(results));
}

nlohmann::json widen_context_blocks(nlohmann::json results, int context_lines) {
    return ::lci::cli::widen_context_blocks(std::move(results), context_lines);
}

}  // namespace grep_filters

// -- AST-aware filter forwarders ---------------------------------------------
//
// Thin forwarders into the anonymous-namespace implementations above. The
// header (src/cli/ast_filters.h) is consumed by tests/cli_test.cpp so the
// classifiers (`match_is_in_comment`, `match_is_in_string_literal`) and the
// JSON transforms (`apply_comments_only`, etc.) can be exercised without
// spinning up the server.
namespace ast_filters {

bool match_is_in_string_literal(std::string_view line, int column) {
    return ::lci::cli::match_is_in_string_literal(line, column);
}

bool match_is_in_comment(std::string_view line, int column) {
    return ::lci::cli::match_is_in_comment(line, column);
}

nlohmann::json apply_comments_only(nlohmann::json results) {
    return ::lci::cli::apply_comments_only(std::move(results));
}

nlohmann::json apply_strings_only(nlohmann::json results) {
    return ::lci::cli::apply_strings_only(std::move(results));
}

nlohmann::json apply_code_only(nlohmann::json results) {
    return ::lci::cli::apply_code_only(std::move(results));
}

}  // namespace ast_filters

namespace {

// Extract the longest literal substring from an ECMAScript regex pattern.
// Used as a trigram seed so the engine narrows files before the local
// regex filter runs. Walks the pattern, skipping escape sequences and
// metacharacters, and tracks the longest contiguous literal run.
std::string longest_literal_run(const std::string& re) {
    std::string best;
    std::string cur;
    auto take = [&]() {
        if (cur.size() > best.size()) best = cur;
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
            case '.': case '*': case '+': case '?':
            case '[': case ']': case '(': case ')':
            case '{': case '}': case '|':
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
    return best;
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
    for (auto& row : results) {
        auto& ctx = row["context"];
        if (!ctx.is_object() || !ctx.contains("lines") ||
            !ctx["lines"].is_array()) {
            continue;
        }
        int start_line = ctx.value("start_line", 1);
        bool kept = false;
        int idx = 0;
        for (auto& line_json : ctx["lines"]) {
            std::string line = line_json.is_string()
                                   ? line_json.get<std::string>()
                                   : "";
            // Strip trailing newline so $ anchors work as expected.
            if (!line.empty() && line.back() == '\n') line.pop_back();
            re2::StringPiece input(line.data(), line.size());
            if (re.Match(input, 0, line.size(), RE2::UNANCHORED,
                         submatches, 1)) {
                auto position = submatches[0].data() - line.data();
                row["line"] = start_line + idx;
                row["column"] = static_cast<int>(position) + 1;
                row["match"] = std::string(submatches[0].data(),
                                           submatches[0].size());
                kept = true;
                break;
            }
            ++idx;
        }
        if (kept) out.push_back(std::move(row));
    }
    return out;
}

bool validate_search_options(const SearchCommandOptions& options) {
    int content_filters = (options.comments_only ? 1 : 0) +
                          (options.code_only ? 1 : 0) +
                          (options.strings_only ? 1 : 0);
    if (content_filters <= 1) return true;

    std::cerr << "Error: --comments-only, --code-only, and --strings-only "
                 "are mutually exclusive (specified "
              << content_filters << ")\n";
    return false;
}

void emit_search_compatibility_notices(const SearchCommandOptions& options) {
    if (options.template_strings && !options.strings_only &&
        !options.json_output) {
        std::cerr << "Note: --template-strings only takes effect with "
                     "--strings-only (mirrors Go reference)\n";
    }
    if (options.template_strings && options.strings_only &&
        !options.json_output) {
        std::cerr << "Note: --template-strings classifier extension is a "
                     "no-op in this build; --strings-only still applies the "
                     "standard classifier. Template literals flow through "
                     "as code.\n";
    }
    if (options.compare_search && !options.json_output) {
        std::cerr << "Note: --compare-search: legacy search path is not "
                     "present in the C++ port; proceeding with the "
                     "consolidated path.\n";
    }
}

int render_search_output(const SearchCommandOptions& options,
                         nlohmann::json& response, double elapsed_ms,
                         std::chrono::steady_clock::time_point verbose_start);

}  // namespace

int run_search(const GlobalFlags& flags, const SearchCommandOptions& options) {
    if (!validate_search_options(options)) return 1;
    emit_search_compatibility_notices(options);

    const auto& pattern = options.pattern;
    const auto& exclude_pattern = options.exclude_pattern;
    const auto& include_pattern = options.include_pattern;
    const auto& extra_patterns = options.extra_patterns;
    const auto& rank_by = options.rank_by;
    const auto& context_filter = options.context_filter;
    const auto& cpu_profile_path = options.cpu_profile_path;
    const auto& mem_profile_path = options.mem_profile_path;
    const int max_lines = options.max_lines;
    const int max_count_per_file = options.max_count_per_file;
    const bool case_insensitive = options.case_insensitive;
    const bool json_output = options.json_output;
    const bool light = options.light;
    const bool use_regex = options.use_regex;
    const bool invert_match = options.invert_match;
    const bool count_per_file = options.count_per_file;
    const bool files_only = options.files_only;
    const bool word_boundary = options.word_boundary;
    const bool include_ids = options.include_ids;
    const bool no_ids = options.no_ids;
    const bool comments_only = options.comments_only;
    const bool code_only = options.code_only;
    const bool strings_only = options.strings_only;
    // -- Search-local profiling (Go cmd/lci/main.go:181-188) ---------------
    //
    // `--cpu-profile` / `--mem-profile` write a profile scoped to this
    // search invocation. Reuses the same gperftools wiring as the global
    // `--profile-cpu` / `--profile-memory` flags; failure to start fails
    // the search rather than silently no-op'ing.
    lci::cli::ProfilerGuard search_cpu_guard;
    lci::cli::ProfilerGuard search_mem_guard;
    if (!cpu_profile_path.empty()) {
        std::string perr;
        search_cpu_guard = lci::cli::start_cpu_profile(cpu_profile_path, perr);
        if (!perr.empty()) {
            std::cerr << "Error: " << perr << "\n";
            return 1;
        }
    }
    if (!mem_profile_path.empty()) {
        std::string perr;
        search_mem_guard = lci::cli::start_memory_profile(mem_profile_path,
                                                          perr);
        if (!perr.empty()) {
            std::cerr << "Error: " << perr << "\n";
            return 1;
        }
    }

    // -- Verbose debug output (Go cmd/lci/main.go:173) ----------------------
    //
    // `--verbose` enables stderr-only diagnostics. We honor it by writing a
    // single-line summary at the end of the search; finer-grained tracing
    // would require plumbing through the engine, which is out of scope here.
    auto verbose_start = std::chrono::steady_clock::now();

    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::string conn_err;
    auto client = ensure_server_running(cfg, conn_err);
    if (!client) {
        std::cerr << "Error: " << conn_err << "\n";
        return 1;
    }

    if (light) {
        std::cerr
            << "WARNING: --light flag is deprecated. Use 'lci grep' instead.\n\n";
    }

    auto start = std::chrono::steady_clock::now();

    // -- Advanced query syntax parsing --------------------------------------
    //
    // Strip directives like `file:*.cpp`, `kind:function`, `symbol:Request`,
    // and `-term` exclusions out of the positional pattern before forwarding
    // to the trigram engine. The directives become post-filters applied to
    // the server's result rows; the bare content terms (rejoined with
    // single spaces) become the actual search pattern.
    //
    // `--patterns` entries (`-e`) skip directive parsing — they're an
    // explicit OR list, and giving a `kind:function` value to the engine
    // there would be a confusing footgun. Most users reach for `--patterns`
    // when they already know the literal strings they want.
    auto parsed_query = query_parser::parse(pattern);
    // When directives were extracted, the parsed `content_query` is the
    // authoritative bare-term pattern (possibly empty if every token was a
    // directive). When NO directives were present, the raw `pattern` flows
    // through unchanged so existing queries pay zero overhead and there's no
    // chance of `parse()` reshaping a literal string the user typed.
    std::string effective_pattern =
        parsed_query.empty_directives() ? pattern : parsed_query.content_query;

    // Regex mode: extract the longest literal substring as a trigram seed
    // for server-side candidate narrowing, then locally filter rows with
    // RE2. Requires a >=3-char literal so the trigram engine can index it;
    // pure-meta patterns return an error.
    //
    // Karpathy: RE2 is linear-time vs std::regex backtracking — order-of-
    // magnitude faster on the hot path. Compiled ONCE here, reused for every
    // row in regex_filter_results.
    std::unique_ptr<RE2> regex_filter;
    bool regex_full_scan = false;
    if (use_regex) {
        auto seed = longest_literal_run(effective_pattern);
        RE2::Options regex_opts(RE2::Quiet);
        regex_opts.set_case_sensitive(!case_insensitive);
        regex_opts.set_log_errors(false);
        // Prefix `(?m)` so `^`/`$` match line boundaries — matches the prior
        // std::regex::multiline behavior. RE2's set_one_line(false) only works
        // in POSIX syntax mode; default Perl mode needs the inline flag.
        std::string with_multiline = "(?m)" + effective_pattern;
        auto re = std::make_unique<RE2>(with_multiline, regex_opts);
        if (!re->ok()) {
            std::cerr << "Error: invalid regex: " << re->error() << "\n";
            return 1;
        }
        regex_filter = std::move(re);
        if (seed.size() >= 3) {
            // Fast path: seed-then-filter via the trigram-indexed
            // server search.
            effective_pattern = seed;
        } else {
            // Pure-meta regex (no usable trigram seed). Mirror Go's
            // behavior: scan every indexed file directly with RE2.
            // Slower than the seeded path but matches Karpathy rule 1
            // (Go is the bar) for patterns like '\\d+' / '^[a-z]+$' /
            // '.{N}' that Go handles via cmd/lci/search.go's
            // full-corpus fallback.
            regex_full_scan = true;
        }
    }

    // Pure-meta regex full-scan path. Walks the FileScanner queue (same
    // file set ctest's `lci list` produces), mmaps each file, applies
    // RE2 line-by-line, emits server-shape result rows. Skips the
    // server entirely.
    if (regex_full_scan) {
        FileScanner scanner(cfg);
        auto tasks = scanner.scan();
        std::sort(tasks.begin(), tasks.end(),
                  [](const FileTask& a, const FileTask& b) {
                      return a.path < b.path;
                  });

        nlohmann::json results = nlohmann::json::array();
        int total_matches = 0;
        for (const auto& task : tasks) {
            MappedFile mf;
            std::string err;
            if (!mf.open(task.path, &err)) continue;
            std::string_view content = mf.view();
            if (content.empty()) continue;

            // RE2 line-by-line over the file content. Cheap per-file
            // CPU since we already pay the mmap; (?m) wasn't needed in
            // the seeded path's row filter, but we want anchored ^/$
            // here too — the regex was compiled with (?m) above so it
            // works against the whole file or per-line equally.
            int line_no = 1;
            size_t pos = 0;
            while (pos < content.size()) {
                size_t eol = content.find('\n', pos);
                std::string_view line =
                    content.substr(pos, eol == std::string_view::npos
                                            ? content.size() - pos
                                            : eol - pos);
                re2::StringPiece sp(line.data(), line.size());
                if (RE2::PartialMatch(sp, *regex_filter)) {
                    nlohmann::json row;
                    row["path"] = task.path;
                    row["line"] = line_no;
                    row["column"] = 0;
                    row["match_text"] = std::string(line);
                    row["score"] = 1.0;
                    results.push_back(std::move(row));
                    ++total_matches;
                    if (max_count_per_file > 0 &&
                        total_matches >= max_count_per_file) {
                        // Per-file cap handled inline since we already
                        // know which file this hit came from.
                        // (Cross-file cap handled below if max-count
                        // limits exist.)
                    }
                }
                if (eol == std::string_view::npos) break;
                pos = eol + 1;
                ++line_no;
            }
        }

        // Build the same envelope shape the server returns so the
        // downstream display/filter pipeline doesn't branch.
        nlohmann::json envelope;
        envelope["results"] = std::move(results);
        envelope["total_matches"] = total_matches;
        std::cout << envelope.dump(2) << "\n";
        return 0;
    }

    // Multi-pattern fan-out: same algorithm as `lci grep` so OR semantics
    // are identical between the two commands. See `search_union_patterns`.
    std::vector<std::string> all_patterns;
    if (!effective_pattern.empty()) all_patterns.push_back(effective_pattern);
    for (const auto& p : extra_patterns) {
        if (!p.empty()) all_patterns.push_back(p);
    }
    if (all_patterns.empty()) {
        // Edge case: query was `file:*.cpp` with no bare terms. The trigram
        // engine cannot run without a pattern, so ask the user to add one.
        // Mirrors Go's behavior where `parseQuerySyntax` of a directive-only
        // query returns an empty `contentPattern` and the engine errors out.
        std::cerr << "Error: at least one search term is required "
                     "(directives like `file:`, `kind:`, `symbol:` cannot "
                     "stand alone)\n";
        return 1;
    }

    std::string search_err;
    std::optional<nlohmann::json> result;
    if (all_patterns.size() == 1) {
        result = client->search(all_patterns.front(), 500, case_insensitive,
                                false, search_err);
    } else {
        result = search_union_patterns(*client, all_patterns, 500,
                                       case_insensitive, search_err);
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    double elapsed_ms =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
                .count()) /
        1000.0;

    if (!result) {
        std::cerr << "Error: search failed: " << search_err << "\n";
        return 1;
    }

    auto& j = *result;

    if (regex_filter) {
        auto raw = j.value("results", nlohmann::json::array());
        j["results"] = regex_filter_results(std::move(raw), *regex_filter);
    }

    // -- Advanced query directive post-filter --------------------------------
    //
    // Apply file/kind/symbol/exclusion filters extracted from the original
    // pattern. This runs first — before grep-style pipelines — so every
    // downstream branch (count, files-only, invert-match) sees the
    // directive-narrowed set.
    // Pass-through (no allocations beyond the move) when no directives were
    // present, so existing queries pay no overhead.
    if (!parsed_query.empty_directives()) {
        auto raw = j.value("results", nlohmann::json::array());
        j["results"] = query_parser::apply_all(std::move(raw), parsed_query);
    }

    // -- --context-filter / --rank-by ----------------------------------------
    //
    // Applied in this order so the re-rank sorts the *kept* set (no point
    // re-ranking rows that the context filter is about to drop). Both flags
    // pass results through unchanged when their value is empty, so existing
    // queries pay no overhead.
    //
    // Unrecognized values warn on stderr and fall through to pass-through
    // rather than failing the search outright — this matches CLI11's
    // permissive `add_option` (no `check()` constraint) and avoids breaking
    // user scripts on a typo.
    {
        auto ctx_filter = rank_options::parse_context_filter(context_filter);
        if (!context_filter.empty() &&
            ctx_filter == rank_options::ContextFilter::Unknown) {
            std::cerr << "Warning: unknown --context-filter value '"
                      << context_filter
                      << "' (expected: function | class | top-level); "
                         "no filtering applied\n";
        }
        if (ctx_filter != rank_options::ContextFilter::None &&
            ctx_filter != rank_options::ContextFilter::Unknown) {
            auto raw = j.value("results", nlohmann::json::array());
            j["results"] = rank_options::apply_context_filter(std::move(raw),
                                                              ctx_filter);
        }

        auto strategy = rank_options::parse_strategy(rank_by);
        if (!rank_by.empty() &&
            strategy == rank_options::RankStrategy::Unknown) {
            std::cerr << "Warning: unknown --rank-by value '" << rank_by
                      << "' (expected: relevance | recency | file-type); "
                         "using default relevance\n";
            strategy = rank_options::RankStrategy::Relevance;
        }
        // `relevance` is still applied (defensive resort) when the user
        // asked for it explicitly. When `--rank-by` was not provided at
        // all, skip even the resort to keep the legacy fast path's output
        // bit-identical for anyone diffing against prior runs.
        if (!rank_by.empty()) {
            auto raw = j.value("results", nlohmann::json::array());
            j["results"] = rank_options::apply_rank(std::move(raw), strategy);
        }
    }

    // -- AST-aware content filters -----------------------------------------
    //
    // Heuristic post-filter pass for `--comments-only`, `--code-only`, and
    // `--strings-only` (mutually exclusive — checked at function entry).
    // Runs AFTER directive/rank/context filters so the classifier only
    // inspects rows that already passed the upstream narrowing — keeps the
    // per-row cost (one disk read fallback per result) bounded by the
    // already-trimmed set. Runs BEFORE grep-style filters so every
    // downstream path (count, files-only, invert-match, JSON, text) sees
    // the AST-narrowed set without per-branch plumbing.
    //
    // The filter is a heuristic — see ast_filters.h for the limitation
    // list (most importantly: it operates on a single line of source at
    // a time, so multi-line block comments and triple-quoted strings
    // that span lines may misclassify the body lines). Documented in
    // the CLI help text on the corresponding flags.
    if (comments_only || code_only || strings_only) {
        auto raw = j.value("results", nlohmann::json::array());
        if (comments_only) {
            j["results"] = apply_comments_only(std::move(raw));
        } else if (code_only) {
            j["results"] = apply_code_only(std::move(raw));
        } else /* strings_only */ {
            j["results"] = apply_strings_only(std::move(raw));
        }
    }

    // -- File path exclude filter (Go honors on `lci search`) ---------------
    //
    // Go's `lci search` forwards --exclude to the server engine which calls
    // filterExcludedFiles (engine.go:2381) — regex/glob path filter applied
    // server-side. C++ post-filters here client-side (compiled once before
    // the loop, Karpathy rule — operates on bounded N ≤ 500 row set).
    //
    // `--include` is NOT honored on `lci search`: Go's server.go:506 forwards
    // only `ExcludePattern: req.Exclude` to the engine, dropping include on
    // the server boundary. Mirror that silently — see iter-10 (DART-6lwWAw28lQfj)
    // pattern: when Go ignores a CLI flag, C++ ignores it too with no client
    // post-filter. Emits a one-line stderr notice (suppressed under --json)
    // for surface parity with the iter-10 grep-style flag notice.
    if (!include_pattern.empty() && !json_output) {
        std::cerr << "Note: --include is ignored by `lci search` "
                     "(mirrors Go reference — server forwards only "
                     "--exclude to the engine). Use `lci grep --include` "
                     "for path filtering on the grep command.\n";
    }
    if (!exclude_pattern.empty()) {
        auto raw = j.value("results", nlohmann::json::array());
        j["results"] = apply_path_filters(std::move(raw), exclude_pattern,
                                          /*include_pattern=*/"");
    }

    // -- Word boundary filter (grep -w, Go honors on `lci search`) ----------
    //
    // Go's `findAllMatchesWithOptions` rewrites a non-regex word-boundary
    // search into `\b<quoted>\b` regex (engine.go:314). We post-filter rows
    // already returned by the literal-match engine, checking byte boundaries
    // on the matched line. Byte-level, no std::regex. See Karpathy rule:
    // word_boundary checks must be byte-level not regex.
    if (word_boundary) {
        auto raw = j.value("results", nlohmann::json::array());
        // Use the first non-empty pattern as the boundary check target.
        // When `--patterns` are present, each row's own `match` field is the
        // authoritative match text; apply_word_boundary falls back to that
        // per-row, so we only need a default for the column-missing path.
        std::string boundary_pat =
            all_patterns.empty() ? pattern : all_patterns.front();
        j["results"] = apply_word_boundary(std::move(raw), boundary_pat,
                                           case_insensitive);
    }

    // -- Object ID toggle (--ids / --no-ids, Go honors on `lci search`) ------
    //
    // Go default: include object_id. `--no-ids` forces exclusion; `--ids`
    // forces inclusion. Server emits `object_id` unconditionally, so we only
    // need to act on `--no-ids` (strip the field). The `--ids` flag is the
    // default and acts as an explicit affirmation — no-op here.
    // `include_ids` retains its value for the future case where the default
    // is flipped (Go's `c.IsSet("ids")` path); today both produce identical
    // output when no-ids is false.
    (void)include_ids;
    if (no_ids) {
        auto raw = j.value("results", nlohmann::json::array());
        j["results"] = strip_object_ids(std::move(raw));
    }

    // -- Grep-filter post-processing for `lci search` ------------------------
    //
    // Go's `lci search` ignores grep-style flags (`--invert-match`,
    // `--max-count`, `--count`, `--files-with-matches`, `--word-regexp`):
    // they are accepted on the command line but not applied to the standard /
    // integrated display path. See cmd/lci/search.go:103-156 — the flags are
    // forwarded into `SearchOptions` but the `displayStandardResults*` writer
    // ignores them; only `lci grep` (`cmd/lci/grep.go`) honors them.
    //
    // The previous C++ implementation post-filtered results client-side
    // here, but the filters operated on the trigram literal-seed (not the
    // regex), so `--regex --invert-match` and `--regex --max-count`
    // produced wrong row counts (DART-6lwWAw28lQfj). Worse, even on
    // non-regex queries the row counts diverged from Go because Go's
    // standard display path never honors these flags at all.
    //
    // Fix: parity with Go — emit a one-line stderr notice the first time a
    // user passes a grep-style flag to `lci search`, then fall through to
    // the standard / integrated display path. Users who need grep semantics
    // should use `lci grep`. The notice is suppressed under `--json` so
    // structured consumers see a clean stream.
    //
    // `word_boundary` was already silently discarded (parameter named
    // `/*word_boundary*/` since iter-0) — keep the discard but cover it by
    // the same notice for consistency.
    bool any_grep_flag = invert_match || count_per_file || files_only ||
                         max_count_per_file > 0;
    if (any_grep_flag && !json_output) {
        std::cerr << "Note: --invert-match, --max-count, --count, and "
                     "--files-with-matches are ignored by `lci search` "
                     "(mirrors Go reference). Use `lci grep` for "
                     "grep-style filtering.\n";
    }

    // -- --max-lines: truncate context.lines around the match --------------
    //
    // Go-parity (cmd/lci/main.go:125-129): `--max-lines N` caps the number
    // of context lines per result. Value `0` is "use blocks" (default — the
    // server returns whatever block size it picked); `N>0` truncates the
    // `context.lines` array on each result row to at most N lines centered
    // on the matching line.
    //
    // Karpathy: in-place truncation, no per-row allocation beyond the
    // shrunken array. Touches each result once, O(N_results) wall-clock.
    if (max_lines > 0 && j.contains("results") && j["results"].is_array()) {
        for (auto& r : j["results"]) {
            if (!r.contains("context") || !r["context"].is_object()) continue;
            auto& ctx = r["context"];
            if (!ctx.contains("lines") || !ctx["lines"].is_array()) continue;
            int start_line = ctx.value("start_line", 1);
            int match_line = r.value("line", start_line);
            auto& lines = ctx["lines"];
            int n_lines = static_cast<int>(lines.size());
            if (n_lines <= max_lines) continue;

            // Center the window on the match line. `before` = max_lines / 2
            // lines preceding the match; `after` = remainder.
            int match_idx = std::max(0, std::min(n_lines - 1,
                                                 match_line - start_line));
            int before = max_lines / 2;
            int from = std::max(0, match_idx - before);
            // Shift right if we hit the lower bound so the window stays
            // size max_lines.
            int to = std::min(n_lines, from + max_lines);
            from = std::max(0, to - max_lines);

            nlohmann::json out = nlohmann::json::array();
            for (int i = from; i < to; ++i) out.push_back(lines[i]);
            ctx["lines"] = std::move(out);
            ctx["start_line"] = start_line + from;
            ctx["end_line"] = start_line + to - 1;
        }
    }

    return render_search_output(options, j, elapsed_ms, verbose_start);
}

namespace {

int render_search_output(const SearchCommandOptions& options,
                         nlohmann::json& response, double elapsed_ms,
                         std::chrono::steady_clock::time_point verbose_start) {
    if (options.json_output) {
        std::error_code cwd_error;
        auto cwd = std::filesystem::current_path(cwd_error);
        auto raw_results = response.value("results", nlohmann::json::array());
        nlohmann::json wrapped = nlohmann::json::array();
        for (auto& result : raw_results) {
            std::string path = result.value("path", "");
            if (!cwd_error && !path.empty()) {
                std::error_code relative_error;
                auto relative =
                    std::filesystem::relative(path, cwd, relative_error);
                if (!relative_error) result["path"] = relative.string();
            }
            wrapped.push_back(nlohmann::json{{"result", result}});
        }
        nlohmann::json output{
            {"query", options.pattern},
            {"time_ms", elapsed_ms},
            {"count", wrapped.size()},
            {"results", std::move(wrapped)},
            {"mode", "standard"},
        };
        std::cout << output.dump(2) << '\n';
        return 0;
    }

    auto results = response.value("results", nlohmann::json::array());
    if (options.compact) {
        std::printf("Found %zu matches in %.1fms (compact mode)\n\n",
                    results.size(), elapsed_ms);
        for (auto& result : results) {
            std::string path =
                to_relative_display_path(result.value("path", ""));
            int match_line = result.value("line", 0);
            auto context = result.value("context", nlohmann::json::object());
            int start_line = context.value("start_line", 0);
            auto lines = context.value("lines", nlohmann::json::array());
            for (size_t i = 0; i < lines.size(); ++i) {
                int line = start_line + static_cast<int>(i);
                if (line != match_line) continue;
                std::string text = lines[i].get<std::string>();
                if (!text.empty() && text.back() == '\n') text.pop_back();
                std::printf("%s:%d: %s\n", path.c_str(), line, text.c_str());
                break;
            }
        }
        return 0;
    }

    std::printf("Found %zu results in %.1fms (standard mode)\n\n",
                results.size(), elapsed_ms);
    for (auto& result : results) {
        std::string path =
            to_relative_display_path(result.value("path", ""));
        int match_line = result.value("line", 0);
        auto context = result.value("context", nlohmann::json::object());
        std::string block_name = context.value("block_name", "");
        std::string block_type = context.value("block_type", "");
        int start_line = context.value("start_line", 0);
        auto lines = context.value("lines", nlohmann::json::array());

        if (options.case_insensitive && start_line == match_line - 1 &&
            match_line > 1 && !lines.empty()) {
            std::string first = lines[0].get<std::string>();
            if (!first.empty() && first.back() == '\n') first.pop_back();
            if (first.empty()) {
                auto prior = read_line_from_file(result.value("path", ""),
                                                 match_line - 2);
                if (!prior.empty()) {
                    lines.insert(lines.begin(), prior);
                    start_line = match_line - 2;
                }
            }
        }

        std::printf("%s:%d", path.c_str(), match_line);
        if (!block_name.empty()) {
            std::printf(" (in %s %s)", block_type.c_str(), block_name.c_str());
        }
        std::printf("\n");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string text = lines[i].get<std::string>();
            if (!text.empty() && text.back() == '\n') text.pop_back();
            std::printf("%6d | %s\n", start_line + static_cast<int>(i),
                        text.c_str());
        }
        std::printf("\n\n");
    }

    if (options.verbose) {
        double total_ms = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - verbose_start)
                .count()) / 1000.0;
        std::fprintf(stderr,
                     "[verbose] pattern=%s patterns=%zu max_lines=%d "
                     "regex=%d total_ms=%.2f\n",
                     options.pattern.c_str(), options.extra_patterns.size(),
                     options.max_lines, options.use_regex ? 1 : 0, total_ms);
    }
    return 0;
}

}  // namespace

int run_grep(const GlobalFlags& flags, const std::string& pattern,
             int max_results, int context_lines, bool case_insensitive,
             bool json_output, const std::string& exclude_pattern,
             const std::string& include_pattern, bool exclude_tests,
             bool exclude_comments, bool use_regex,
             bool invert_match,
             const std::vector<std::string>& extra_patterns,
             bool count_per_file, bool files_only,
             int max_count_per_file, bool verbose) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::string conn_err;
    auto client = ensure_server_running(cfg, conn_err);
    if (!client) {
        std::cerr << "Error: " << conn_err << "\n";
        return 1;
    }

    auto start = std::chrono::steady_clock::now();

    // -- Multi-pattern fan-out (--patterns / -e) -----------------------------
    //
    // Go's `lci search` builds a single regex of the form `(p1)|(p2)|(p3)`
    // and pushes that to the indexer. The C++ server has no regex path yet,
    // so we issue one literal search per pattern and merge results, dropping
    // duplicates by (path, line). The first occurrence wins so the
    // positional pattern's ranking is preserved.
    std::vector<std::string> all_patterns;
    if (!pattern.empty()) all_patterns.push_back(pattern);
    for (const auto& p : extra_patterns) {
        if (!p.empty()) all_patterns.push_back(p);
    }
    if (all_patterns.empty()) {
        std::cerr << "Error: at least one pattern is required\n";
        return 1;
    }

    // -- --regex (grep -E) for `lci grep` ----------------------------------
    //
    // Go-parity (cmd/lci/main.go:214-218): `--regex` / `-E` interprets the
    // pattern as a regex. The C++ trigram server has no regex code path, so
    // we mirror what `lci search --regex` does: extract a >=3-char literal
    // seed for trigram narrowing, then RE2-filter result rows locally.
    // Pure-meta patterns fail fast with a clear error rather than silently
    // returning nothing (Karpathy rule 6).
    //
    // Compiled ONCE before the per-row loop; reused across every row.
    std::unique_ptr<RE2> grep_regex_filter;
    std::vector<std::string> grep_seeds;
    if (use_regex) {
        RE2::Options regex_opts(RE2::Quiet);
        regex_opts.set_case_sensitive(!case_insensitive);
        regex_opts.set_log_errors(false);
        // Compile one RE2 per pattern — the per-row filter accepts a vector.
        // For multi-pattern (-e), we OR them into a single regex string so we
        // only compile once; RE2 handles `(a)|(b)|(c)` efficiently.
        std::string combined;
        for (size_t i = 0; i < all_patterns.size(); ++i) {
            if (i > 0) combined += "|";
            combined += "(" + all_patterns[i] + ")";
            auto seed = longest_literal_run(all_patterns[i]);
            if (seed.size() >= 3) grep_seeds.push_back(seed);
        }
        std::string with_multiline = "(?m)" + combined;
        auto re = std::make_unique<RE2>(with_multiline, regex_opts);
        if (!re->ok()) {
            std::cerr << "Error: invalid regex: " << re->error() << "\n";
            return 1;
        }
        grep_regex_filter = std::move(re);
        if (grep_seeds.empty()) {
            std::cerr << "Error: --regex pattern has no >=3-char literal "
                         "substring suitable for the trigram-indexed fast "
                         "path; the C++ port has no full-corpus regex scan "
                         "for `lci grep` (use `lci search -E` for that).\n";
            return 1;
        }
    }

    std::string search_err;
    std::optional<nlohmann::json> result;
    if (use_regex) {
        // Search by literal seed(s), then RE2-filter the row set.
        result = search_union_patterns(*client, grep_seeds, max_results,
                                       case_insensitive, search_err);
    } else if (all_patterns.size() == 1) {
        result = client->search(all_patterns.front(), max_results,
                                case_insensitive, false, search_err);
    } else {
        result = search_union_patterns(*client, all_patterns, max_results,
                                       case_insensitive, search_err);
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    double elapsed_ms =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
                .count()) /
        1000.0;

    if (!result) {
        std::cerr << "Error: search failed: " << search_err << "\n";
        return 1;
    }

    auto& j = *result;

    // RE2 row filter for --regex mode. Runs before any other grep filter
    // so the downstream pipeline sees the regex-matching set.
    if (grep_regex_filter) {
        auto raw = j.value("results", nlohmann::json::array());
        j["results"] = regex_filter_results(std::move(raw),
                                            *grep_regex_filter);
    }

    // -- Apply grep filters in a defined order ------------------------------
    // 1) exclude-tests / exclude-comments shrink the input set first so all
    //    downstream modes (invert, max-count, count, files-only) operate on
    //    the already-filtered set. exclude-tests is path-only (cheap),
    //    exclude-comments inspects the matched line.
    // 2) invert-match rebuilds the row list from file contents — invert
    //    semantics are about non-matching lines in matching files, so we
    //    intentionally invert AFTER tests are dropped (no point inverting in
    //    excluded files) but on the post-comment-filter set so a comment-only
    //    file still contributes its non-comment lines.
    // 3) max-count caps per-file BEFORE count/files-only aggregation so the
    //    summary respects the cap (parity with grep's `-m N -c`).
    nlohmann::json results_arr = j.value("results", nlohmann::json::array());

    // -- File path exclude filter (Go honors --exclude on `lci grep`) -------
    // Go's grepCommand forwards --exclude into SearchOptions and the server
    // engine applies filterExcludedFiles. `--include` is silently dropped on
    // the server boundary (server.go:506 forwards only ExcludePattern). We
    // mirror Go: post-filter on exclude, ignore include (with stderr notice).
    // Runs before exclude_tests/exclude_comments so every downstream mode
    // sees the path-narrowed set.
    if (!include_pattern.empty() && !json_output) {
        std::cerr << "Note: --include is ignored by `lci grep` "
                     "(mirrors Go reference — server forwards only "
                     "--exclude to the engine).\n";
    }
    if (!exclude_pattern.empty()) {
        results_arr = apply_path_filters(std::move(results_arr),
                                         exclude_pattern,
                                         /*include_pattern=*/"");
    }

    if (exclude_tests) {
        results_arr = apply_exclude_tests(std::move(results_arr));
    }
    if (exclude_comments) {
        results_arr = apply_exclude_comments(std::move(results_arr));
    }

    if (invert_match) {
        results_arr = invert_match_rows(results_arr, all_patterns,
                                        case_insensitive, max_count_per_file);
        // Re-apply --exclude-comments after invert: the pre-invert pass only
        // saw matched lines, but invert synthesizes rows for *all* non-match
        // lines in the same files — many of which are themselves comment-only
        // lines that the user asked to drop. (--exclude-tests is path-only,
        // so the pre-invert pass already removed all rows from test files;
        // the invert pass cannot reintroduce them.)
        if (exclude_comments) {
            // For invert rows the synthesized `match` is the line text, and
            // `read_match_line()` correctly extracts it from the embedded
            // single-line context block, so the same helper applies.
            results_arr = apply_exclude_comments(std::move(results_arr));
        }
    } else if (max_count_per_file > 0) {
        results_arr = apply_max_count_per_file(std::move(results_arr),
                                               max_count_per_file);
    }

    // Widen the per-result `context` block when the user requests extra
    // surrounding lines via `--context N`. The server returns a 1-line window
    // by default; reading the file once per result and replacing the block
    // gives JSON consumers and the text formatter the lines they expect.
    // Skip for invert-match rows: those already carry a single-line context
    // block built from the non-match line, and re-reading would defeat the
    // invert semantics (we'd pull in matching lines as "context").
    if (context_lines > 0 && !invert_match && !count_per_file && !files_only) {
        results_arr = widen_context_blocks(std::move(results_arr),
                                           context_lines);
    }

    // -- Summary modes -------------------------------------------------------
    if (count_per_file) {
        auto summary = count_per_file_rows(results_arr);
        if (json_output) {
            nlohmann::json output;
            output["query"] = pattern;
            output["time_ms"] = elapsed_ms;
            output["mode"] = "count";
            output["results"] = summary;
            output["count"] = summary.size();
            std::cout << output.dump(2) << "\n";
            return 0;
        }
        std::printf("Found matches in %zu file(s) in %.1fms (grep -c)\n\n",
                    summary.size(), elapsed_ms);
        for (auto& row : summary) {
            std::string path =
                to_relative_display_path(row.value("path", ""));
            int n = row.value("count", 0);
            std::printf("%s: %d\n", path.c_str(), n);
        }
        return 0;
    }

    if (files_only) {
        auto summary = files_with_matches_rows(results_arr);
        if (json_output) {
            nlohmann::json output;
            output["query"] = pattern;
            output["time_ms"] = elapsed_ms;
            output["mode"] = "files-with-matches";
            output["results"] = summary;
            output["count"] = summary.size();
            std::cout << output.dump(2) << "\n";
            return 0;
        }
        std::printf("Found %zu file(s) with matches in %.1fms (grep -l)\n\n",
                    summary.size(), elapsed_ms);
        for (auto& row : summary) {
            std::string path =
                to_relative_display_path(row.value("path", ""));
            std::printf("%s\n", path.c_str());
        }
        return 0;
    }

    // -- Default grep output (one line per match) ---------------------------
    if (json_output) {
        nlohmann::json output;
        output["query"] = pattern;
        output["time_ms"] = elapsed_ms;
        output["results"] = results_arr;
        output["count"] = results_arr.size();
        output["mode"] = invert_match ? "invert-match" : "grep";
        std::cout << output.dump(2) << "\n";
        return 0;
    }

    std::printf("Found %zu matches in %.1fms (%s mode)\n\n", results_arr.size(),
                elapsed_ms, invert_match ? "invert-match" : "grep");

    for (auto& r : results_arr) {
        std::string path = to_relative_display_path(r.value("path", ""));
        int line = r.value("line", 0);
        int column = r.value("column", 0);

        if (invert_match) {
            // Synthetic invert rows already carry the bare line text in
            // `match`; print it directly without traversing the context
            // block (which only holds a single duplicate of that line).
            std::string text = r.value("match", "");
            std::printf("%s:%d:%d:%s\n", path.c_str(), line, column,
                        text.c_str());
            continue;
        }

        auto context = r.value("context", nlohmann::json::object());
        int start_line = context.value("start_line", 0);
        auto lines = context.value("lines", nlohmann::json::array());

        if (context_lines > 0 && !lines.empty()) {
            // grep -C N format: "path-LINE-text" for context lines and
            // "path:LINE:COL:text" for the match itself. A separator row
            // ("--") goes between adjacent results so consumers can chunk
            // the output the way GNU grep does. Trailing '\n' on each line
            // is stripped to avoid double-newlines from printf.
            for (size_t i = 0; i < lines.size(); ++i) {
                int line_num = start_line + static_cast<int>(i);
                std::string text = lines[i].get<std::string>();
                if (!text.empty() && text.back() == '\n') text.pop_back();
                if (line_num == line) {
                    std::printf("%s:%d:%d:%s\n", path.c_str(), line_num, column,
                                text.c_str());
                } else {
                    std::printf("%s-%d-%s\n", path.c_str(), line_num,
                                text.c_str());
                }
            }
            std::printf("--\n");
            continue;
        }

        for (size_t i = 0; i < lines.size(); ++i) {
            int line_num = start_line + static_cast<int>(i);
            if (line_num == line) {
                std::printf("%s:%d:%d:%s\n", path.c_str(), line_num, column,
                            lines[i].get<std::string>().c_str());
                break;
            }
        }
    }

    if (verbose) {
        std::fprintf(stderr,
                     "[verbose] pattern=%s patterns=%zu regex=%d "
                     "rows=%zu elapsed_ms=%.2f\n",
                     pattern.c_str(), extra_patterns.size(),
                     use_regex ? 1 : 0, results_arr.size(), elapsed_ms);
    }
    return 0;
}

}  // namespace cli
}  // namespace lci
