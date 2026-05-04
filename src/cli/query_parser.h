// Advanced query syntax parser for `lci search`.
//
// Parses structured query expressions like:
//
//   file:*.cpp kind:function auth
//   symbol:Request -test
//   file:src/* kind:class -deprecated Foo
//
// Splits the raw query string into:
//   - bare terms        : forwarded to the trigram engine as the search pattern
//   - file glob         : `file:<glob>`         restrict result paths
//   - kind filter       : `kind:<symbolkind>`   restrict to enclosing block_type
//   - symbol filter     : `symbol:<name>`       restrict to enclosing block_name
//   - exclusion terms   : `-<term>`             drop results whose match line contains term
//
// All directives are space-separated; bare tokens (no `directive:` prefix and
// not starting with `-`) become content terms. Multiple directives of the same
// kind compose by AND-ing (file glob is a single value — last write wins; the
// Go reference also accepts only one `path:` glob per query). When the query
// contains no directives the parser returns the original string unchanged in
// `content_query` and all directive vectors empty, so the caller can short-
// circuit straight to the trigram engine.
//
// This is a header-only module so unit tests in tests/cli_test.cpp can
// exercise it without linking the CLI binary. The parser does not read any
// files or talk to the server — it is pure string manipulation, plus a few
// JSON post-filter helpers that match against the server's result rows.
//
// Reference: lci Go's `parseQuerySyntax()` at
// internal/indexing/master_index_search.go:159 — that implementation supports
// `path:`, `dir:`, `ext:` (file targeting only). lci-cpp adds `kind:`,
// `symbol:` (semantic targeting on the result row's enclosing-scope metadata)
// and `-<term>` exclusions, since those are the directives users have asked
// for. The set is deliberately small: extending the syntax is cheap once the
// parsing harness exists.
//
// IMPORTANT — `kind:` and `symbol:` rely on the server populating
// `context.block_type` and `context.block_name` on each result row. The
// current lci-cpp search engine (src/search/engine.cpp) does NOT compute
// per-file block boundaries and so leaves both fields empty for almost all
// queries — meaning `kind:function` will yield zero results until the engine
// gains block-aware context extraction. The parser still does the correct
// thing semantically: when block metadata starts arriving from the server,
// the post-filter will narrow results without further code changes here.
// `file:` and `-<term>` exclusions work today because they only inspect
// fields the server already populates (`path` and `match`).

#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lci {
namespace cli {
namespace query_parser {

/// Parsed view of a query string. Constructed by `parse()`. All vectors
/// preserve the order tokens appeared in the input, so error messages and
/// debug output can echo the user's intent. `content_query` is the joined
/// bare-term tokens (space-separated), already trimmed of leading/trailing
/// whitespace; when empty the caller should treat the query as "filter-only"
/// and decide whether to error out (the trigram engine cannot run without a
/// pattern).
struct ParsedQuery {
    std::string content_query;            // bare terms, space-joined
    std::string file_glob;                // file:<glob> — last write wins
    std::vector<std::string> kinds;       // kind:<...>
    std::vector<std::string> symbols;     // symbol:<...>
    std::vector<std::string> exclusions;  // -<term>

    /// True when no directives were extracted — caller can pass `content_query`
    /// straight through to the engine without post-filtering.
    bool empty_directives() const {
        return file_glob.empty() && kinds.empty() && symbols.empty() &&
               exclusions.empty();
    }
};

/// Returns true if `prefix` matches the start of `s` (byte-wise compare).
inline bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

/// Splits `query` on whitespace, preserving non-empty tokens in order.
/// Multiple consecutive separators collapse — empty tokens are not emitted.
/// Whitespace is the C-locale `isspace` set; quoted tokens are not handled
/// (the Go reference uses `strings.Fields` and accepts no quoting either,
/// which we mirror to keep parity).
inline std::vector<std::string> split_whitespace(std::string_view query) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < query.size()) {
        while (i < query.size() &&
               std::isspace(static_cast<unsigned char>(query[i]))) {
            ++i;
        }
        size_t start = i;
        while (i < query.size() &&
               !std::isspace(static_cast<unsigned char>(query[i]))) {
            ++i;
        }
        if (i > start) {
            tokens.emplace_back(query.substr(start, i - start));
        }
    }
    return tokens;
}

/// Lowercases an ASCII string. Non-ASCII bytes pass through untouched. Used
/// for case-insensitive comparison of `kind:` and `symbol:` directive values
/// against server-supplied metadata fields. Kept private to this header — the
/// equivalent helper in search.cpp is in a translation-unit-local namespace.
inline std::string ascii_lower(std::string_view s) {
    std::string out;
    out.resize(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(s[i])));
    }
    return out;
}

/// Parses `query` into a `ParsedQuery`. Tokens with a recognized
/// `<directive>:` prefix populate the matching vector; tokens starting with
/// `-` and at least one trailing character become exclusions; everything
/// else is collected as a bare content term. The input is treated as a
/// snapshot — the function does not modify any external state.
///
/// Recognized directives (case-sensitive on the directive prefix; values are
/// preserved as written so glob and symbol matching can be exact):
///
///   - `file:<glob>`     — single glob pattern. Multiple `file:` directives
///                         keep only the last (matches Go's `path:`
///                         single-value semantics; users that want OR should
///                         use a brace glob like `file:*.{cpp,hpp}`).
///   - `kind:<kindname>` — repeats are kept as separate entries; matched as
///                         OR by the post-filter (a result is kept when its
///                         block_type matches any provided kind).
///   - `symbol:<name>`   — repeats are kept as separate entries; matched as
///                         OR (a result is kept when its block_name matches
///                         any provided symbol substring, case-insensitive).
///   - `-<term>`         — exclusion: drop results whose match line contains
///                         the term (case-insensitive). A bare `-` (no term)
///                         is treated as a content token to avoid silently
///                         eating user input.
///
/// Tokens with a colon but an unrecognized directive prefix (e.g. `foo:bar`)
/// fall through to the content-term bucket. This is intentional: the trigram
/// engine indexes literal characters, so a query like `http://example` should
/// stay a single content term, not be misclassified as the unknown `http:`
/// directive.
inline ParsedQuery parse(std::string_view query) {
    ParsedQuery out;
    auto tokens = split_whitespace(query);
    std::vector<std::string> content_tokens;
    content_tokens.reserve(tokens.size());

    for (auto& tok : tokens) {
        if (starts_with(tok, "file:")) {
            out.file_glob = tok.substr(5);  // last write wins
            continue;
        }
        if (starts_with(tok, "kind:")) {
            std::string val = tok.substr(5);
            if (!val.empty()) out.kinds.push_back(std::move(val));
            continue;
        }
        if (starts_with(tok, "symbol:")) {
            std::string val = tok.substr(7);
            if (!val.empty()) out.symbols.push_back(std::move(val));
            continue;
        }
        if (tok.size() >= 2 && tok.front() == '-') {
            out.exclusions.push_back(tok.substr(1));
            continue;
        }
        content_tokens.push_back(std::move(tok));
    }

    // Join content tokens with single spaces. The trigram engine treats the
    // entire pattern as a literal substring search, so re-joining preserves
    // multi-word phrases like `error code` exactly as the user typed them.
    for (size_t i = 0; i < content_tokens.size(); ++i) {
        if (i > 0) out.content_query.push_back(' ');
        out.content_query.append(content_tokens[i]);
    }

    return out;
}

// -- Glob matching -----------------------------------------------------------
//
// fnmatch-lite for `file:` directives. Supports `*` (zero or more chars,
// non-greedy implementation via recursion) and `?` (exactly one char). Brace
// expansion is not implemented — users that need OR can issue separate
// queries, matching Go's single-glob semantics.
//
// Matching is case-sensitive and runs against the path's basename when the
// glob contains no `/`, otherwise the full path. This mirrors `fnmatch(3)`
// with no `FNM_PATHNAME` flag for short globs while still letting
// `file:src/cli/*.cpp` work as expected.

/// True if `pattern` matches `text` using `*` / `?` glob semantics.
/// Recursive descent — fine for short patterns (paths, globs).
inline bool glob_match(std::string_view pattern, std::string_view text) {
    size_t p = 0, t = 0;
    size_t star_p = std::string_view::npos;  // last `*` position in pattern
    size_t star_t = 0;                        // text position when `*` was seen

    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star_p = p++;
            star_t = t;
        } else if (p < pattern.size() &&
                   (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (star_p != std::string_view::npos) {
            // Backtrack: extend the most recent `*` by one char.
            p = star_p + 1;
            t = ++star_t;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

/// Convenience: applies `glob_match` against either the basename or the full
/// path depending on whether the pattern contains a `/`. Empty patterns
/// match everything (used for "no file: directive" pass-through).
inline bool path_matches_glob(std::string_view pattern,
                              std::string_view path) {
    if (pattern.empty()) return true;
    if (pattern.find('/') == std::string_view::npos) {
        // Pattern with no slash matches the basename only.
        auto slash = path.find_last_of('/');
        std::string_view base =
            (slash == std::string_view::npos) ? path : path.substr(slash + 1);
        return glob_match(pattern, base);
    }
    return glob_match(pattern, path);
}

// -- Result post-filters -----------------------------------------------------
//
// These take a JSON `results` array (the server's /search response shape) and
// return a new array with rows that pass the directive's predicate. They are
// pure (no I/O), composable, and stable — they preserve input ordering for
// kept rows. Each helper short-circuits on an empty directive so callers can
// chain them unconditionally without first checking `empty_directives()`.

/// Drops rows whose `path` does not match `glob`. Pass-through when `glob`
/// is empty.
inline nlohmann::json apply_file_filter(nlohmann::json results,
                                        const std::string& glob) {
    if (glob.empty()) return results;
    nlohmann::json out = nlohmann::json::array();
    for (auto& r : results) {
        std::string p = r.value("path", "");
        if (path_matches_glob(glob, p)) {
            out.push_back(std::move(r));
        }
    }
    return out;
}

/// Drops rows whose `context.block_type` does not match any entry in `kinds`
/// (case-insensitive). Pass-through when `kinds` is empty.
inline nlohmann::json apply_kind_filter(nlohmann::json results,
                                        const std::vector<std::string>& kinds) {
    if (kinds.empty()) return results;
    std::vector<std::string> lowered;
    lowered.reserve(kinds.size());
    for (const auto& k : kinds) lowered.push_back(ascii_lower(k));

    nlohmann::json out = nlohmann::json::array();
    for (auto& r : results) {
        auto ctx = r.value("context", nlohmann::json::object());
        std::string bt = ctx.value("block_type", "");
        std::string bt_lower = ascii_lower(bt);
        bool keep = false;
        for (const auto& k : lowered) {
            if (bt_lower == k) {
                keep = true;
                break;
            }
        }
        if (keep) out.push_back(std::move(r));
    }
    return out;
}

/// Drops rows whose `context.block_name` does not contain any entry in
/// `symbols` as a case-insensitive substring. Pass-through when `symbols` is
/// empty.
///
/// Uses substring (not exact) match so that `symbol:Request` finds both
/// `Request` and `RequestHandler` — matches the user expectation of
/// "results in any symbol whose name contains 'Request'".
inline nlohmann::json apply_symbol_filter(
    nlohmann::json results, const std::vector<std::string>& symbols) {
    if (symbols.empty()) return results;
    std::vector<std::string> lowered;
    lowered.reserve(symbols.size());
    for (const auto& s : symbols) lowered.push_back(ascii_lower(s));

    nlohmann::json out = nlohmann::json::array();
    for (auto& r : results) {
        auto ctx = r.value("context", nlohmann::json::object());
        std::string bn = ctx.value("block_name", "");
        std::string bn_lower = ascii_lower(bn);
        bool keep = false;
        for (const auto& s : lowered) {
            if (!s.empty() && bn_lower.find(s) != std::string::npos) {
                keep = true;
                break;
            }
        }
        if (keep) out.push_back(std::move(r));
    }
    return out;
}

/// Drops rows whose `match` line contains any of `terms` (case-insensitive
/// substring match). Also checks the row's `path` so a query like `-test`
/// excludes results from a file named `auth_test.cpp` even if the matched
/// line itself is symbol-only (matches the user's intuition of
/// "exclude anything mentioning test"). Pass-through when `terms` is empty.
inline nlohmann::json apply_exclusion_filter(
    nlohmann::json results, const std::vector<std::string>& terms) {
    if (terms.empty()) return results;
    std::vector<std::string> lowered;
    lowered.reserve(terms.size());
    for (const auto& t : terms) {
        if (!t.empty()) lowered.push_back(ascii_lower(t));
    }
    if (lowered.empty()) return results;

    nlohmann::json out = nlohmann::json::array();
    for (auto& r : results) {
        std::string match_text = r.value("match", "");
        std::string path = r.value("path", "");
        std::string match_lower = ascii_lower(match_text);
        std::string path_lower = ascii_lower(path);
        bool drop = false;
        for (const auto& term : lowered) {
            if (match_lower.find(term) != std::string::npos ||
                path_lower.find(term) != std::string::npos) {
                drop = true;
                break;
            }
        }
        if (!drop) out.push_back(std::move(r));
    }
    return out;
}

/// Convenience: applies all four directive filters in a defined order.
/// Order matters for performance (cheapest first) but the final set is
/// associative, so the result is independent of ordering. Sequence:
///   1. file glob (path-only, no field lookups)
///   2. kind (single field lookup per row)
///   3. symbol (single field lookup per row)
///   4. exclusions (two field lookups per row)
inline nlohmann::json apply_all(nlohmann::json results,
                                const ParsedQuery& q) {
    if (q.empty_directives()) return results;
    results = apply_file_filter(std::move(results), q.file_glob);
    results = apply_kind_filter(std::move(results), q.kinds);
    results = apply_symbol_filter(std::move(results), q.symbols);
    results = apply_exclusion_filter(std::move(results), q.exclusions);
    return results;
}

}  // namespace query_parser
}  // namespace cli
}  // namespace lci
