// Internal helpers for `lci symbols` filter and sort flags. Exposed via this
// header so unit tests in tests/cli_test.cpp can exercise the pure logic
// without going through the full CLI/server pipeline. Not part of the public
// API.
//
// Helpers exposed:
//
//   - `is_glob_pattern(s)` — Returns true if `s` contains glob metacharacters
//     (`*`, `?`, `[`). Mirrors the heuristic used by gitignore.cpp:68 to
//     distinguish a substring filter from a glob.
//
//   - `glob_match(pattern, text)` — Single-segment glob match. `*` matches
//     any run of non-`/` characters, `?` matches any single non-`/`
//     character. Matches Go's `filepath.Match` semantics for the simple
//     forms used by `lci symbols --file`.
//
//   - `glob_match_path_or_basename(pattern, path)` — Two-step match used by
//     the Go server's `handleListSymbols` (server.go:803-810): try the full
//     path first, then the basename. So `--file "*.go"` matches
//     `internal/foo.go` via the basename leg.
//
//   - `apply_file_glob(symbols, pattern)` — Drops symbol entries whose
//     `file` field doesn't satisfy `glob_match_path_or_basename`. Stable
//     order. Pass-through when `pattern` is empty.
//
//   - `sort_symbols(symbols, sort_key)` — Stable sort over the input array
//     by the named key. Supported keys: `name` (default), `complexity`
//     (descending), `refs` (incoming + outgoing, descending), `line`
//     (ascending, secondary by file path), `params` (descending). Unknown
//     keys are treated as `name`. Pass-through when `sort_key` is empty.
//
//   - `apply_max_limit(symbols, max_results)` — Truncates the array to at
//     most `max_results` entries. Pass-through when `max_results <= 0`.

#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lci {
namespace cli {
namespace symbol_filters {

/// Returns true if `s` contains any glob metacharacter (`*`, `?`, `[`).
/// A bare substring (no metacharacters) returns false and is intended to be
/// passed straight to the server's substring-based file filter.
bool is_glob_pattern(std::string_view s);

/// Single-segment glob match. `*` matches any run of non-`/` characters,
/// `?` matches any single non-`/` character. Returns true on full match.
/// Designed to mirror Go's `filepath.Match` for the simple forms accepted
/// by `lci symbols --file`.
bool glob_match(std::string_view pattern, std::string_view text);

/// Two-step matcher mirroring the Go server's `handleListSymbols`
/// (server.go:803-810): tries `glob_match(pattern, path)` first, then
/// `glob_match(pattern, basename(path))`. Returns true on either hit.
bool glob_match_path_or_basename(std::string_view pattern,
                                 std::string_view path);

/// Filters out symbol entries whose `file` field doesn't match the glob.
/// Stable order. Pass-through when `pattern` is empty.
nlohmann::json apply_file_glob(nlohmann::json symbols,
                               std::string_view pattern);

/// Stable sort over the symbols array by the named key. Supported keys:
///   - `name`        — ascending by name (default for empty/unknown keys)
///   - `complexity`  — descending by complexity
///   - `refs`        — descending by incoming_refs + outgoing_refs
///   - `line`        — ascending by file then line
///   - `params`      — descending by parameter_count
/// Pass-through when `sort_key` is empty.
nlohmann::json sort_symbols(nlohmann::json symbols, std::string_view sort_key);

/// Truncates the symbols array to at most `max_results` entries.
/// Pass-through when `max_results <= 0`.
nlohmann::json apply_max_limit(nlohmann::json symbols, int max_results);

}  // namespace symbol_filters
}  // namespace cli
}  // namespace lci
