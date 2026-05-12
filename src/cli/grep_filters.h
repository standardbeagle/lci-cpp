// Internal helpers for `lci grep` filter flags. Exposed via this header so
// unit tests in tests/cli_test.cpp can exercise the pure logic without going
// through the full CLI/server pipeline. Not part of the public API.
//
// Three filters are exposed:
//
//   - `line_looks_like_comment(line)` — Go parity heuristic from
//     internal/search/engine.go:1804. Returns true if the trimmed line
//     starts with `//`, `#`, or `/*`, or contains `*/` anywhere.
//
//   - `path_is_test(path)` — Returns true if the path matches any test-file
//     convention (`_test.`, `.test.`, `.spec.`, `test_*`, trailing `Test`/
//     `Tests` suffix, or a `tests/` / `test/` directory component).
//
//   - `apply_exclude_tests`, `apply_exclude_comments`, `widen_context_blocks`
//     — JSON transforms over the server's `results` array. Pure functions
//     modulo filesystem reads (the comment/context filters open files
//     referenced by `path` to inspect line text).

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lci {
namespace cli {
namespace grep_filters {

/// Returns true if the trimmed `line` looks like it starts inside or contains
/// a comment token. Mirrors Go's `Engine.isInComment` logic
/// (internal/search/engine.go:1804): a line is considered "in a comment" if,
/// after trimming leading/trailing whitespace, it starts with `//`, `#`, or
/// `/*`, or anywhere contains `*/`. A deliberately cheap heuristic that
/// matches Go bit-for-bit so `--exclude-comments` produces the same drop-set
/// across both binaries.
bool line_looks_like_comment(std::string_view line);

/// Returns true if `path` looks like a test file. Recognizes the basename
/// patterns handled by `lci::is_test_file` (`_test.`, `.test.`, `.spec.`,
/// `test_*`) plus the trailing `Test`/`Tests` suffix used by GoogleTest-style
/// C++ files (`FooTest.cpp`, `FooTests.cpp`), and any path that has a
/// `tests/` or `test/` directory component. Paths in lci's pipeline are
/// normalized to forward slashes.
bool path_is_test(std::string_view path);

/// Filters out result rows whose path matches `path_is_test()`. Stable order
/// (preserves the input array's ordering for non-dropped rows).
nlohmann::json apply_exclude_tests(nlohmann::json results);

/// Filters out result rows whose match line looks like it lives inside a
/// comment. Reads the match line from the embedded `context` block when
/// possible, falling back to a disk read of `path:line`. Stable order.
nlohmann::json apply_exclude_comments(nlohmann::json results);

/// Replaces each result's `context` block with one that spans
/// `[line - context_lines, line + context_lines]` read fresh from disk.
/// Pass-through when `context_lines <= 0`.
nlohmann::json widen_context_blocks(nlohmann::json results, int context_lines);

}  // namespace grep_filters
}  // namespace cli
}  // namespace lci
