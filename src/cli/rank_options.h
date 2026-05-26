// Ranking and context-filter helpers for `lci search`.
//
// Two CLI flags are wired into this header:
//
//   --rank-by <strategy>      Re-rank results before they're displayed.
//   --context-filter <ctx>    Drop results whose enclosing context doesn't
//                             match the filter.
//
// Both are header-only so unit tests in tests/cli_test.cpp can exercise the
// pure logic without spinning up the server. The CLI-binary wiring (in
// src/cli/search.cpp) is a thin call into these helpers after the server
// returns its result rows.
//
// -- --rank-by -----------------------------------------------------------
//
// The trigram engine assigns each result a `score` (sum of base + bonuses
// from `kCodeFileBoost`, `kWordBoundaryBonus`, etc.). `--rank-by` lets the
// user pick a *different* strategy at runtime without recompiling those
// constants. Strategies:
//
//   relevance  : default. Pass-through — server scores already reflect the
//                relevance constants, so no client-side reweighting needed.
//                Stable sort by score descending (ties keep server order).
//                The strategy is recognized so users can request it
//                explicitly (e.g. for scripts that switch between modes).
//
//   recency    : sort by file mtime (newest first). Each result row's
//                `path` is stat'd once; the resulting epoch second value
//                replaces the score for sort purposes. Files we can't
//                stat (deleted, missing, permission errors) sink to the
//                bottom with mtime=0 — degraded but non-fatal.
//
//   file-type  : re-score using the existing `score_file_type()` helper
//                (kCodeFileBoost / kDocFilePenalty / kConfigFileBoost) and
//                sort by the new score descending. Other server-side
//                bonuses (word boundary, line start, etc.) are discarded
//                so this strategy is *purely* file-extension driven —
//                that's the user-facing contract: "rank by file type".
//                Original score is preserved as `original_score` for JSON
//                consumers that want to recover the engine's view.
//
// Unknown strategies fall through to relevance (warned on stderr) so
// typos don't fail the search outright.
//
// -- --context-filter ----------------------------------------------------
//
// Post-filter that keeps only results whose `context.block_type` /
// `context.block_name` match the requested context kind. Three contexts:
//
//   function   : keep when block_type is "function" or "method".
//   class      : keep when block_type is "class", "struct", "interface",
//                "trait", "impl", or "record" (every "class-shaped"
//                container — matches users' intuition of "inside a class
//                body" regardless of language).
//   top-level  : keep when block_type is empty, "lines", or "context"
//                (the engine's "no enclosing scope resolved" sentinels).
//
// Unknown context filters fall through to pass-through (warned on stderr).
// Empty filter = no filtering at all.
//
// IMPORTANT — like `kind:`/`symbol:` directives in query_parser.h, the
// context filter relies on the server populating `context.block_type` on
// each row. The lci-cpp engine populates this field from
// engine_context.cpp:145 / 188 / 225, but only when the search hit a file
// with parseable symbols. Hits in unparseable files (e.g. plain text)
// end up with block_type="lines" and are routed through the top-level
// bucket, which is consistent with treating them as "outside any enclosing
// definition".

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <lci/search/search_options.h>

namespace lci {
namespace cli {
namespace rank_options {

// -- Strategy parsing --------------------------------------------------------

/// Recognized ranking strategies. `Unknown` means a typo or empty string —
/// the caller should pass through (no reweighting) and may warn the user.
enum class RankStrategy : uint8_t {
    Relevance = 0,
    Recency,
    FileType,
    Unknown,
};

/// Recognized context filters. `None` = empty input (no filtering at all).
/// `Unknown` = the user typed something we don't recognize; caller should
/// pass through and may warn.
enum class ContextFilter : uint8_t {
    None = 0,
    Function,
    Class,
    TopLevel,
    Unknown,
};

/// Lowercases an ASCII string. Mirrors the helper in query_parser.h so
/// recognition is case-insensitive ("Recency", "RECENCY", "recency" all
/// resolve to RankStrategy::Recency).
inline std::string ascii_lower(std::string_view s) {
    std::string out;
    out.resize(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(s[i])));
    }
    return out;
}

/// Parses a `--rank-by` value into a strategy. Empty and "relevance" both
/// yield `Relevance` (the no-op default). Hyphen and underscore are
/// interchangeable so users can type `file-type` or `file_type`.
///
/// Go-parity aliases (decision recorded in S3 CLI parity task, _rationale below):
///   "proximity"   → Relevance  — Go ranks by token-locality; the C++
///                   port has no separate locality scorer, so we route
///                   to Relevance (the closest existing strategy: the
///                   server's word-boundary/line-start bonuses give a
///                   locality-like signal). Accepting the alias keeps
///                   `--rank-by proximity` from failing user scripts.
///   "similarity"  → Relevance  — Go's similarity ranker is rapidfuzz-based
///                   over symbol names; no equivalent on the search hot
///                   path in C++ today. Fallback to Relevance is honest:
///                   the user asked for similarity ordering, gets the
///                   default relevance ordering. Aliased rather than
///                   silently dropped so the flag is accepted.
///
/// _rationale: Aliasing rather than implementing avoids adding two new
/// scoring code paths to the read-side hot loop (Karpathy rule 3). The
/// alias is documented in --help text and in any parity descriptors that
/// exercise --rank-by.
inline RankStrategy parse_strategy(std::string_view raw) {
    if (raw.empty()) return RankStrategy::Relevance;
    std::string lower = ascii_lower(raw);
    // Normalize hyphen/underscore to a single canonical form.
    for (char& c : lower) {
        if (c == '_') c = '-';
    }
    if (lower == "relevance") return RankStrategy::Relevance;
    if (lower == "recency") return RankStrategy::Recency;
    if (lower == "file-type") return RankStrategy::FileType;
    // Go-parity aliases — route to the nearest existing C++ behavior.
    if (lower == "proximity") return RankStrategy::Relevance;
    if (lower == "similarity") return RankStrategy::Relevance;
    return RankStrategy::Unknown;
}

/// Parses a `--context-filter` value. Empty -> `None`. Unknown values
/// resolve to `Unknown` so the caller can warn the user; the safe
/// fallback in that case is to pass results through unchanged.
inline ContextFilter parse_context_filter(std::string_view raw) {
    if (raw.empty()) return ContextFilter::None;
    std::string lower = ascii_lower(raw);
    for (char& c : lower) {
        if (c == '_') c = '-';
    }
    if (lower == "function" || lower == "method" || lower == "func") {
        return ContextFilter::Function;
    }
    if (lower == "class" || lower == "struct" || lower == "interface" ||
        lower == "trait" || lower == "impl" || lower == "record") {
        return ContextFilter::Class;
    }
    if (lower == "top-level" || lower == "toplevel" || lower == "top" ||
        lower == "global") {
        return ContextFilter::TopLevel;
    }
    return ContextFilter::Unknown;
}

// -- Recency: file mtime lookup ---------------------------------------------

/// Returns the file's last-modified time in seconds since the Unix epoch,
/// or 0 if the file doesn't exist or can't be stat'd. Pure helper so
/// tests can swap in their own time source via temporary files. Uses the
/// system clock (POSIX `mtime`) — same source Go's `os.Stat()` reports.
inline int64_t file_mtime_epoch(const std::string& path) {
    if (path.empty()) return 0;
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    // file_clock -> system_clock conversion. C++20 provides
    // `clock_cast`, but it's still patchy across our supported toolchains
    // (libstdc++ 13 in the build matrix); the duration arithmetic below
    // is portable and produces the right epoch-second value for sort
    // purposes (we don't need calendar precision, only relative order).
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(
               sctp.time_since_epoch())
        .count();
}

// -- Re-rankers --------------------------------------------------------------
//
// Each helper takes a JSON `results` array and returns a new array with
// rows reordered (and, for `file-type`, with `score` rewritten and
// `original_score` preserved). All helpers are stable: when two rows
// have equal sort keys, their input order is preserved so callers see
// deterministic output.

/// Sorts rows by `score` descending (relevance no-op assert: server
/// already returns sorted results, but we re-sort defensively in case
/// upstream merging — e.g. `--patterns` fan-out — left things unsorted).
inline nlohmann::json apply_relevance(nlohmann::json results) {
    std::vector<nlohmann::json> rows;
    rows.reserve(results.size());
    for (auto& r : results) rows.push_back(std::move(r));
    std::stable_sort(rows.begin(), rows.end(),
                     [](const nlohmann::json& a, const nlohmann::json& b) {
                         double sa = a.value("score", 0.0);
                         double sb = b.value("score", 0.0);
                         return sa > sb;
                     });
    nlohmann::json out = nlohmann::json::array();
    for (auto& r : rows) out.push_back(std::move(r));
    return out;
}

/// Sorts rows by file mtime descending (newest file first). Per-row
/// mtime is stamped onto the row as `mtime_epoch` so JSON consumers can
/// see the value the sort used. Files that fail to stat get
/// `mtime_epoch: 0` and sink to the bottom.
inline nlohmann::json apply_recency(nlohmann::json results) {
    // Cache mtime per path so files appearing in multiple result rows
    // pay one stat() per file, not one per row.
    std::vector<std::pair<int64_t, nlohmann::json>> stamped;
    stamped.reserve(results.size());
    std::unordered_map<std::string, int64_t> mtime_cache;
    for (auto& r : results) {
        std::string path = r.value("path", "");
        int64_t mtime = 0;
        if (!path.empty()) {
            auto it = mtime_cache.find(path);
            if (it == mtime_cache.end()) {
                mtime = file_mtime_epoch(path);
                mtime_cache.emplace(path, mtime);
            } else {
                mtime = it->second;
            }
        }
        r["mtime_epoch"] = mtime;
        stamped.emplace_back(mtime, std::move(r));
    }
    std::stable_sort(stamped.begin(), stamped.end(),
                     [](const auto& a, const auto& b) {
                         return a.first > b.first;
                     });
    nlohmann::json out = nlohmann::json::array();
    for (auto& [_, r] : stamped) out.push_back(std::move(r));
    return out;
}

/// Re-scores every row using `score_file_type(path)` and sorts by the
/// new score descending. Saves the original engine score as
/// `original_score` so JSON consumers can recover the engine view if
/// they need it.
///
/// Rationale: kCodeFileBoost (50.0), kConfigFileBoost (10.0),
/// kDocFilePenalty (-20.0) are the three deltas this strategy applies.
/// The relative ordering matches what the engine already does when
/// summing `score_file_type` into the total — this strategy strips
/// every other component (word boundary, exact case, etc.) so the
/// ranking is purely "code over config over docs", which is what users
/// who pick `--rank-by file-type` are asking for.
inline nlohmann::json apply_file_type(nlohmann::json results) {
    std::vector<std::pair<double, nlohmann::json>> rescored;
    rescored.reserve(results.size());
    for (auto& r : results) {
        std::string path = r.value("path", "");
        double new_score = lci::score_file_type(path);
        if (r.contains("score")) {
            r["original_score"] = r["score"];
        }
        r["score"] = new_score;
        rescored.emplace_back(new_score, std::move(r));
    }
    std::stable_sort(rescored.begin(), rescored.end(),
                     [](const auto& a, const auto& b) {
                         return a.first > b.first;
                     });
    nlohmann::json out = nlohmann::json::array();
    for (auto& [_, r] : rescored) out.push_back(std::move(r));
    return out;
}

/// Convenience dispatcher: routes to the strategy-specific helper. For
/// `Relevance` this is still a defensive resort (cheap when rows are
/// already ordered). For `Unknown` it's a pass-through so an unrecognized
/// flag value doesn't drop results (the caller is expected to print a
/// warning).
inline nlohmann::json apply_rank(nlohmann::json results, RankStrategy strategy) {
    switch (strategy) {
        case RankStrategy::Relevance:
            return apply_relevance(std::move(results));
        case RankStrategy::Recency:
            return apply_recency(std::move(results));
        case RankStrategy::FileType:
            return apply_file_type(std::move(results));
        case RankStrategy::Unknown:
        default:
            return results;
    }
}

// -- Context filter ----------------------------------------------------------

/// True when `bt` (a `block_type` string from the server) matches the
/// requested filter. Pure helper — exposed so tests can pin the matrix
/// of (filter, block_type) -> bool independent of the JSON wrangling.
///
/// Block-type strings come from `to_string(SymbolType)` in
/// include/lci/types.h: "function", "method", "class", "struct",
/// "interface", "trait", "impl", "record", and the engine sentinels
/// "lines" and "context" used when no enclosing scope was resolved.
inline bool block_type_matches(ContextFilter filter, std::string_view bt) {
    std::string lower = ascii_lower(bt);
    switch (filter) {
        case ContextFilter::Function:
            return lower == "function" || lower == "method" ||
                   lower == "constructor";
        case ContextFilter::Class:
            return lower == "class" || lower == "struct" ||
                   lower == "interface" || lower == "trait" ||
                   lower == "impl" || lower == "record";
        case ContextFilter::TopLevel:
            // "Top-level" means the result lives outside any enclosing
            // function/class/etc. The engine signals that with an empty
            // block_type or one of two sentinels ("lines" — synthesized
            // by master_index_search.cpp:160 when no scope was resolved,
            // "context" — engine_context.cpp:225 fallback).
            return lower.empty() || lower == "lines" || lower == "context";
        case ContextFilter::None:
        case ContextFilter::Unknown:
        default:
            // Pass-through: every block_type "matches" when there's no
            // active filter. apply_context_filter() short-circuits before
            // this is ever asked.
            return true;
    }
}

/// Drops result rows whose `context.block_type` doesn't match `filter`.
/// Pass-through when the filter is `None` or `Unknown`. Stable order.
inline nlohmann::json apply_context_filter(nlohmann::json results,
                                           ContextFilter filter) {
    if (filter == ContextFilter::None || filter == ContextFilter::Unknown) {
        return results;
    }
    nlohmann::json out = nlohmann::json::array();
    for (auto& r : results) {
        auto ctx = r.value("context", nlohmann::json::object());
        std::string bt = ctx.value("block_type", "");
        if (block_type_matches(filter, bt)) {
            out.push_back(std::move(r));
        }
    }
    return out;
}

}  // namespace rank_options
}  // namespace cli
}  // namespace lci
