// AST-aware content filters for `lci search`. Heuristic post-filters that
// classify each match by token kind (comment / string-literal / code) and
// drop rows that don't fit the requested scope. Exposed via this header so
// unit tests in tests/cli_test.cpp can exercise the pure logic without
// going through the full CLI/server pipeline. Not part of the public API.
//
// Three filters are exposed (all mutually exclusive at the CLI layer; this
// header just provides the per-row predicates and JSON transforms):
//
//   - `apply_comments_only(results)`  -> keep only matches inside comment
//     tokens. Reuses `grep_filters::line_looks_like_comment()` so the
//     classifier matches `--exclude-comments` bit-for-bit (a line that's
//     dropped by `--exclude-comments` is exactly a line that's KEPT by
//     `--comments-only`).
//
//   - `apply_strings_only(results)`   -> keep only matches whose match column
//     falls inside a string literal on the source line. Uses a single-pass
//     scanner over the line that respects backslash escapes, single-line
//     // and # comments (a quote inside a comment doesn't open a string),
//     and triple quotes ("""...""") for Python-style multiline literals.
//     Block comments (/* ... */) are recognized when both delimiters appear
//     on the same line — multi-line block comments are a documented
//     limitation since the post-filter only sees one line at a time.
//
//   - `apply_code_only(results)`      -> exclude rows that are either
//     comments OR string-literal matches. Equivalent to running both of the
//     above as drop-filters in sequence.
//
// All three transforms preserve input ordering for non-dropped rows and
// fall back gracefully when row metadata is missing: a row without a
// readable line text is passed through (so an indexer hiccup doesn't
// silently drop everything), and a row without a column number is
// classified by line-level heuristics only.
//
// IMPORTANT: these are HEURISTIC filters, not true AST parsing. They run
// over a single line of source at a time and don't track multi-line
// context (e.g. a `/* ... */` block spanning three lines, or a Python
// triple-quoted string spanning ten). The classifier intentionally errs
// toward the Go reference behavior so `lci-cpp` and `lci` Go produce the
// same drop-set on the same corpus.

#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lci {
namespace cli {
namespace ast_filters {

/// Returns true if the byte at `column` (1-based) on `line` falls inside a
/// string literal. Quote scanner walks the line left-to-right tracking
/// state (in_single_quote, in_double_quote, in_triple_quote, in_block_comment),
/// honors backslash escapes inside single/double quotes, and bails out
/// early when a single-line comment opens (// or # outside a string —
/// after that point everything to end-of-line is comment, not code or
/// string). A column at or past the line length returns false.
///
/// Triple-quote detection covers Python's `"""..."""` and `'''...'''`. A
/// triple-quote opener consumes the matching triple-quote closer; partial
/// triple quotes that don't close on the same line are treated as still
/// open through end-of-line (so a match anywhere after the opener is
/// considered "in a string"). Block-comment detection handles `/* ... */`
/// when both ends appear on the same line; partial block comments behave
/// like single-line comments after their opener.
///
/// `column` is 1-based to match the indexer's convention; pass 0 to ask
/// for the leftmost-column match position. Negative columns return false.
bool match_is_in_string_literal(std::string_view line, int column);

/// Returns true if the byte at `column` (1-based) on `line` falls inside a
/// comment token. A line-leading `//`, `#`, or `/*` makes EVERY column on
/// the line a comment (the whole line is comment body). When a single-line
/// comment opens partway through the line (`code(); // tail`), only columns
/// at or past the comment opener are considered comment bytes — so a match
/// on `code()` is NOT a comment, but a match on `tail` IS. Same logic for
/// `/*` openers when no `*/` closes on the same line.
///
/// `*/` ANYWHERE on the line marks every column up to and including the
/// `*/` as comment (the line crossed a block-comment closer, so its head is
/// the comment tail). Mirrors the existing `line_looks_like_comment()`
/// heuristic from grep_filters.h but with column-level granularity.
bool match_is_in_comment(std::string_view line, int column);

/// Filters `results` to keep only rows whose match falls inside a comment
/// token. Reads the match line via the embedded `context` block (or disk
/// fallback) and the match column via the row's `column` field. Stable
/// order. Rows missing path/line are passed through (graceful degradation).
nlohmann::json apply_comments_only(nlohmann::json results);

/// Filters `results` to keep only rows whose match falls inside a string
/// literal. Reads the match line and column the same way as
/// `apply_comments_only`. Stable order; missing-metadata rows pass through.
nlohmann::json apply_strings_only(nlohmann::json results);

/// Filters `results` to drop rows whose match is either a comment OR a
/// string-literal match. Equivalent to applying the inverse of each of
/// the above filters in sequence. Stable order; missing-metadata rows
/// pass through.
nlohmann::json apply_code_only(nlohmann::json results);

}  // namespace ast_filters
}  // namespace cli
}  // namespace lci
