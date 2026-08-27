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
#include <re2/re2.h>

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

/// Literal seeds for the regex fast path: EVERY literal run of >=3 chars in
/// the pattern, deduplicated, in appearance order. The former single
/// longest-run seed was blind to alternation — for
/// `\b(?:panic|unreachable|todo|unimplemented)!\s*\(` it seeded only
/// "unimplemented", so every `panic!` site was silently absent from the
/// result set (measured: 1 of 1203 real sites on a fixture; the pattern is
/// err-lookup's production Rust detector). Seeding the union of all runs
/// keeps every row containing ANY run; the RE2 row filter then decides.
/// Still heuristic for patterns whose only runs are optional (`x(?:abc)?`),
/// which can match text containing no run at all — same bound as before,
/// now over a strictly larger row set.
std::vector<std::string> regex_literal_seeds(const std::string& pattern);

/// Resolves the trailing `lci grep/search <path>...` positionals to the
/// ROOT-relative form the index matches against. Each token is interpreted as
/// absolute or relative to `cwd`, then expressed relative to `root`. Purely
/// lexical (no disk access). A token that escapes `root` is returned
/// unchanged so the server-side index-membership check can reject it loudly.
std::vector<std::string> resolve_scope_paths(
    const std::vector<std::string>& paths, const std::string& root,
    const std::string& cwd);


// Row-level result transforms and file/path utilities shared by run_search /
// run_grep after the per-command split (search.cpp / grep.cpp). Same
// test-visibility rationale as the filters above.
std::string to_relative_display_path(const std::string& path);
std::string read_line_from_file(const std::string& path, int line_number);
std::string read_match_line(const nlohmann::json& result,
                            const std::string& path, int line_no);
nlohmann::json invert_match_rows(const nlohmann::json& results,
                                 const std::vector<std::string>& patterns,
                                 bool case_insensitive,
                                 int max_count_per_file);
nlohmann::json apply_word_boundary(nlohmann::json results,
                                   const std::string& pattern,
                                   bool case_insensitive);
nlohmann::json apply_path_filters(nlohmann::json results,
                                  const std::string& exclude_pattern,
                                  const std::string& include_pattern);
nlohmann::json strip_object_ids(nlohmann::json results);
nlohmann::json apply_max_count_per_file(nlohmann::json results,
                                        int max_count_per_file);
nlohmann::json count_per_file_rows(const nlohmann::json& results);
nlohmann::json files_with_matches_rows(const nlohmann::json& results);
nlohmann::json regex_filter_results(nlohmann::json results, const RE2& re);

}  // namespace grep_filters
}  // namespace cli
}  // namespace lci
