// Implementation of the AST-aware content filters declared in ast_filters.h
// (--comments-only / --code-only / --strings-only). Split out of search.cpp.

#include "ast_filters.h"

#include "grep_filters.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace lci {
namespace cli {

using namespace grep_filters;

namespace ast_filters {

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

}  // namespace ast_filters
}  // namespace cli
}  // namespace lci
