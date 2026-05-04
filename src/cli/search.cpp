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
#include <lci/search/search_options.h>

#include "grep_filters.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
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

/// Formats a breadcrumb segment for enhanced/assembly output. Returns
/// `<type> <name>` for the typical case where the engine resolved both
/// fields, falling back to whichever single field is populated. Empty when
/// nothing is known. Mirrors Go's `(in %s %s)` formatter.
std::string format_breadcrumb_segment(std::string_view block_type,
                                      std::string_view block_name) {
    if (block_type.empty() && block_name.empty()) return {};
    if (block_type.empty()) return std::string(block_name);
    if (block_name.empty()) return std::string(block_type);
    std::string out;
    out.reserve(block_type.size() + 1 + block_name.size());
    out.append(block_type);
    out.push_back(' ');
    out.append(block_name);
    return out;
}

/// Formats the metrics line shown by `--enhanced`. Joins the non-zero
/// fields with ` | ` in the order Go uses (complexity, lines, refs) and
/// returns an empty string when no field is positive.
std::string format_metrics_line_text(int complexity, int lines_of_code,
                                     int reference_count) {
    std::string out;
    auto append = [&](const std::string& part) {
        if (!out.empty()) out.append(" | ");
        out.append(part);
    };
    if (complexity > 0) {
        append("complexity: " + std::to_string(complexity));
    }
    if (lines_of_code > 0) {
        append("lines: " + std::to_string(lines_of_code));
    }
    if (reference_count > 0) {
        append("refs: " + std::to_string(reference_count));
    }
    return out;
}

/// Reads the enclosing symbol bounds (start/end line) from the result row's
/// `enclosing_start`/`enclosing_end` fields (populated upstream when symbol
/// metadata is available) and replaces the `context` block with the full
/// `[enclosing_start, enclosing_end]` range read from disk. Rows missing
/// either field are passed through. The matched line's number is preserved
/// in `matched_lines`; `start_line`/`end_line` reflect the actual lines
/// read (may be shorter than the requested range when the file ends early).
nlohmann::json widen_to_enclosing_blocks(nlohmann::json results) {
    for (auto& r : results) {
        std::string path = r.value("path", "");
        int line_no = r.value("line", 0);
        int from = r.value("enclosing_start", 0);
        int to = r.value("enclosing_end", 0);
        if (path.empty() || line_no <= 0 || from <= 0 || to < from) continue;

        auto lines = read_lines_range(path, from, to);
        if (lines.empty()) continue;

        nlohmann::json ctx;
        ctx["block_type"] = r.value("context",
                                    nlohmann::json::object()).value(
            "block_type", "lines");
        ctx["block_name"] = r.value("context",
                                    nlohmann::json::object()).value(
            "block_name", "");
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

/// Looks up the enclosing symbol's complexity, lines_of_code, end_line, and
/// reference counts via `browse_file` and annotates every result row whose
/// (file, block_name) hits a match in the file's symbol list. Cached per file
/// so multiple results in the same file share one HTTP round-trip.
///
/// The annotations are added under stable keys directly on the result row:
///   - `enclosing_complexity`  : int, omitted when zero
///   - `enclosing_lines`       : int (lines_of_code), omitted when zero
///   - `enclosing_refs`        : int (incoming_refs), omitted when zero
///   - `enclosing_start`       : int (symbol declaration line)
///   - `enclosing_end`         : int (symbol end line, when known)
///
/// On lookup failure the row is left unchanged (graceful degradation; we
/// don't fail the whole search just because metrics aren't available).
nlohmann::json annotate_with_symbol_metrics(Client& client,
                                            nlohmann::json results) {
    // Cache: file path -> JSON browse-file response (or null on failure so
    // we don't retry on every row).
    std::map<std::string, nlohmann::json> file_cache;
    for (auto& r : results) {
        std::string path = r.value("path", "");
        if (path.empty()) continue;
        auto context = r.value("context", nlohmann::json::object());
        std::string block_name = context.value("block_name", "");
        if (block_name.empty()) continue;

        auto it = file_cache.find(path);
        if (it == file_cache.end()) {
            BrowseFileRequest req;
            req.file = path;
            req.max = 1000;
            std::string err;
            auto resp = client.browse_file(req, err);
            file_cache[path] = resp.value_or(nlohmann::json());
            it = file_cache.find(path);
        }
        if (it->second.is_null()) continue;

        auto symbols = it->second.value("symbols", nlohmann::json::array());
        for (const auto& s : symbols) {
            if (s.value("name", "") != block_name) continue;
            int sline = s.value("line", 0);
            int eline = s.value("end_line", 0);
            int cx = s.value("complexity", 0);
            int loc = s.value("lines_of_code", 0);
            int incoming = s.value("incoming_refs", 0);
            if (cx > 0) r["enclosing_complexity"] = cx;
            if (loc > 0) r["enclosing_lines"] = loc;
            if (incoming > 0) r["enclosing_refs"] = incoming;
            if (sline > 0) r["enclosing_start"] = sline;
            if (eline > 0) r["enclosing_end"] = eline;
            break;
        }
    }
    return results;
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

std::string format_breadcrumb(std::string_view block_type,
                              std::string_view block_name) {
    return ::lci::cli::format_breadcrumb_segment(block_type, block_name);
}

std::string format_metrics_line(int complexity, int lines_of_code,
                                int reference_count) {
    return ::lci::cli::format_metrics_line_text(complexity, lines_of_code,
                                                reference_count);
}

nlohmann::json widen_to_enclosing_block(nlohmann::json results) {
    return ::lci::cli::widen_to_enclosing_blocks(std::move(results));
}

}  // namespace grep_filters

int run_search(const GlobalFlags& flags, const std::string& pattern,
               int max_lines, bool case_insensitive, bool json_output,
               bool light, bool compact_search, bool /*use_regex*/,
               const std::string& /*exclude_pattern*/,
               const std::string& /*include_pattern*/,
               bool invert_match,
               const std::vector<std::string>& extra_patterns,
               bool count_per_file,
               bool files_only, bool /*word_boundary*/,
               int max_count_per_file, bool /*include_ids*/,
               bool /*no_ids*/, bool /*comments_only*/, bool /*code_only*/,
               bool /*strings_only*/, const std::string& /*rank_by*/,
               const std::string& /*context_filter*/,
               bool enhanced, bool assembly) {
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

    // Multi-pattern fan-out: same algorithm as `lci grep` so OR semantics
    // are identical between the two commands. See `search_union_patterns`.
    std::vector<std::string> all_patterns;
    if (!pattern.empty()) all_patterns.push_back(pattern);
    for (const auto& p : extra_patterns) {
        if (!p.empty()) all_patterns.push_back(p);
    }
    if (all_patterns.empty()) {
        std::cerr << "Error: at least one pattern is required\n";
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

    // -- Grep-filter post-processing for `lci search` ------------------------
    //
    // `lci search` and `lci grep` share the same five filter flags; running
    // them through one helper keeps behavior identical between the two
    // commands. Order matters:
    //   1. invert -> rebuild the result list from raw file contents.
    //   2. max-count -> cap per-file BEFORE count/files-only aggregate.
    //   3. count / files-with-matches -> reduce to summary rows.
    //
    // A bare `--max-count` without invert/count/files-only is handled by the
    // standard formatter below — we only need to trim the result list before
    // it's displayed, not switch into the grep-shaped path.
    if (max_count_per_file > 0 && !invert_match && !count_per_file &&
        !files_only) {
        auto trimmed = apply_max_count_per_file(
            j.value("results", nlohmann::json::array()), max_count_per_file);
        j["results"] = std::move(trimmed);
    }

    bool any_filter = invert_match || count_per_file || files_only;

    if (any_filter) {
        nlohmann::json results_arr = j.value("results", nlohmann::json::array());

        if (invert_match) {
            results_arr = invert_match_rows(results_arr, all_patterns,
                                            case_insensitive,
                                            max_count_per_file);
        } else if (max_count_per_file > 0) {
            results_arr = apply_max_count_per_file(std::move(results_arr),
                                                   max_count_per_file);
        }

        nlohmann::json summary_arr;
        std::string mode_label = "standard";
        bool is_summary = false;
        if (count_per_file) {
            summary_arr = count_per_file_rows(results_arr);
            mode_label = "count";
            is_summary = true;
        } else if (files_only) {
            summary_arr = files_with_matches_rows(results_arr);
            mode_label = "files-with-matches";
            is_summary = true;
        }

        if (json_output) {
            nlohmann::json output;
            output["query"] = pattern;
            output["time_ms"] = elapsed_ms;
            output["mode"] = mode_label;
            if (is_summary) {
                // --count -> [{path,count}, ...]
                // --files-with-matches -> [{path}, ...]
                output["results"] = summary_arr;
                output["count"] = summary_arr.size();
            } else {
                // --invert-match (and bare --max-count) keep the wrapped
                // result shape so JSON consumers can parse a single schema.
                std::error_code rel_ec;
                auto cwd = std::filesystem::current_path(rel_ec);
                nlohmann::json wrapped = nlohmann::json::array();
                for (auto& r : results_arr) {
                    std::string path = r.value("path", "");
                    if (!rel_ec && !path.empty()) {
                        std::error_code ec;
                        auto rel = std::filesystem::relative(path, cwd, ec);
                        if (!ec) r["path"] = rel.string();
                    }
                    wrapped.push_back(nlohmann::json{{"result", r}});
                }
                output["results"] = wrapped;
                output["count"] = wrapped.size();
            }
            std::cout << output.dump(2) << "\n";
            return 0;
        }

        if (is_summary) {
            if (count_per_file) {
                std::printf("Found matches in %zu file(s) in %.1fms (count "
                            "mode)\n\n",
                            summary_arr.size(), elapsed_ms);
                for (auto& row : summary_arr) {
                    std::string path =
                        to_relative_display_path(row.value("path", ""));
                    int n = row.value("count", 0);
                    std::printf("%s: %d\n", path.c_str(), n);
                }
            } else {
                std::printf("Found %zu file(s) with matches in %.1fms (files-"
                            "with-matches mode)\n\n",
                            summary_arr.size(), elapsed_ms);
                for (auto& row : summary_arr) {
                    std::string path =
                        to_relative_display_path(row.value("path", ""));
                    std::printf("%s\n", path.c_str());
                }
            }
            return 0;
        }

        // --invert-match (or bare --max-count) text mode: emit grep-style
        // path:line:text rows so downstream tooling (editors, scripts) can
        // parse them like normal grep output.
        std::printf("Found %zu lines in %.1fms (%s mode)\n\n",
                    results_arr.size(), elapsed_ms,
                    invert_match ? "invert-match" : "standard");
        for (auto& r : results_arr) {
            std::string path = to_relative_display_path(r.value("path", ""));
            int line = r.value("line", 0);
            std::string text = r.value("match", "");
            std::printf("%s:%d:%s\n", path.c_str(), line, text.c_str());
        }
        return 0;
    }

    // -- Enhanced / assembly output modes -----------------------------------
    //
    // Both modes layer on top of the standard pipeline: they reuse the same
    // upstream filtering and only diverge at the formatting step. They are
    // mutually exclusive (a stricter form of Go's `displayEnhancedResults` vs
    // `displayStandardResultsWithAssembly`); when both flags are set,
    // `--enhanced` wins because its output is a strict superset of assembly's
    // (it includes the surrounding-block context plus metrics and breadcrumbs).
    //
    // For both modes we annotate each row with the enclosing symbol's
    // metrics via `browse_file` (cached per file). For `--assembly` we also
    // widen the context block to span the enclosing symbol's full
    // `[line, end_line]` range when the lookup succeeded — that's the
    // "surrounding function/class context" the task spec requires.
    if (enhanced || assembly) {
        nlohmann::json results_arr =
            j.value("results", nlohmann::json::array());

        if (max_count_per_file > 0) {
            results_arr = apply_max_count_per_file(std::move(results_arr),
                                                   max_count_per_file);
        }

        // Annotate every row with `enclosing_*` fields when symbol metrics
        // are resolvable. Failure to resolve is silent — rows just render
        // without metrics, exactly like Go's enhanced path skips the metrics
        // line when `RelationalData.Symbol.Metrics` is nil.
        results_arr =
            annotate_with_symbol_metrics(*client, std::move(results_arr));

        if (assembly) {
            // Widen each row's context block to the enclosing symbol's
            // bounds. Rows without resolved bounds keep their server-default
            // single-line window (graceful degradation matches Go's
            // assembly-disabled path which just renders standard results).
            results_arr = widen_to_enclosing_blocks(std::move(results_arr));
        }

        if (json_output) {
            // Wrapping shape mirrors `--json` standard mode for editor
            // consumers; the `mode` field switches so callers can identify
            // the schema. `enhanced` mode adds top-level `assembly_triggered:
            // false` to advertise that the C++ build does not run an
            // AssemblySearchEngine (parity-friendly stub for Go consumers).
            std::error_code rel_ec;
            auto cwd = std::filesystem::current_path(rel_ec);
            nlohmann::json wrapped = nlohmann::json::array();
            for (auto& r : results_arr) {
                std::string path = r.value("path", "");
                if (!rel_ec && !path.empty()) {
                    std::error_code ec;
                    auto rel = std::filesystem::relative(path, cwd, ec);
                    if (!ec) r["path"] = rel.string();
                }
                wrapped.push_back(nlohmann::json{{"result", r}});
            }
            nlohmann::json output;
            output["query"] = pattern;
            output["time_ms"] = elapsed_ms;
            output["count"] = wrapped.size();
            output["results"] = wrapped;
            output["mode"] = enhanced ? "enhanced" : "integrated";
            if (assembly && !enhanced) {
                // Match Go's `displayStandardResultsWithAssembly` JSON shape:
                // explicit advertise that no assembly fragments are
                // available so consumers can disambiguate from a "we just
                // failed to build them" case.
                output["assembly_triggered"] = false;
                output["assembly_count"] = 0;
                output["assembly_matches"] = nlohmann::json::array();
                output["search_mode"] = "integrated";
            }
            std::cout << output.dump(2) << "\n";
            return 0;
        }

        if (compact_search) {
            // Compact mode is identical to standard compact: one line per
            // match. The flags don't affect compact output (Go behaves the
            // same: `displayStandardResultsWithAssembly` short-circuits to
            // the compact branch before printing breadcrumbs/metrics).
            std::printf("Found %zu matches in %.1fms (compact mode)\n\n",
                        results_arr.size(), elapsed_ms);
            for (auto& r : results_arr) {
                std::string path =
                    to_relative_display_path(r.value("path", ""));
                int line = r.value("line", 0);
                auto context = r.value("context", nlohmann::json::object());
                int start_line = context.value("start_line", 0);
                auto lines = context.value("lines", nlohmann::json::array());
                for (size_t i = 0; i < lines.size(); ++i) {
                    int line_num = start_line + static_cast<int>(i);
                    if (line_num == line) {
                        std::string text = lines[i].get<std::string>();
                        if (!text.empty() && text.back() == '\n') text.pop_back();
                        std::printf("%s:%d: %s\n", path.c_str(), line_num,
                                    text.c_str());
                        break;
                    }
                }
            }
            return 0;
        }

        const char* header_mode = enhanced ? "enhanced" : "integrated";
        if (enhanced) {
            std::printf("Found %zu enhanced results in %.1fms\n\n",
                        results_arr.size(), elapsed_ms);
        } else {
            std::printf("Found %zu results in %.1fms (integrated mode)\n",
                        results_arr.size(), elapsed_ms);
            std::printf("  Direct matches: %zu\n", results_arr.size());
            std::printf("  Assembly patterns: 0\n\n");
            std::puts("=== Direct Matches ===");
        }
        (void)header_mode;

        for (auto& r : results_arr) {
            std::string path = to_relative_display_path(r.value("path", ""));
            int line = r.value("line", 0);
            auto context = r.value("context", nlohmann::json::object());
            std::string block_type = context.value("block_type", "");
            std::string block_name = context.value("block_name", "");
            int start_line = context.value("start_line", 0);
            auto lines = context.value("lines", nlohmann::json::array());

            // Header: `path:line` plus optional breadcrumb segment. We gate
            // on `block_name` being populated to mirror Go's
            // displayEnhancedResults at search.go:632 (`if r.Context.BlockName
            // != ""`) — `block_type=lines` with no name is the engine's "no
            // enclosing scope resolved" sentinel, and Go suppresses the
            // breadcrumb in that case rather than printing `(in lines)`.
            std::printf("%s:%d", path.c_str(), line);
            if (!block_name.empty()) {
                std::string crumb =
                    format_breadcrumb_segment(block_type, block_name);
                std::printf(" (in %s)", crumb.c_str());
            }

            if (enhanced) {
                int incoming = r.value("enclosing_refs", 0);
                int outgoing = 0;  // not surfaced by browse-file today
                if (incoming > 0 || outgoing > 0) {
                    std::printf(" [refs: %d incoming, %d outgoing]", incoming,
                                outgoing);
                }
            }
            std::printf("\n");

            // Context lines: enhanced uses Go's `> NNNN | text` format with a
            // marker on the matched line; assembly mode reuses the standard
            // formatter (six-char-padded line number + ` | ` + text).
            for (size_t i = 0; i < lines.size(); ++i) {
                int line_num = start_line + static_cast<int>(i);
                std::string text = lines[i].get<std::string>();
                if (!text.empty() && text.back() == '\n') text.pop_back();
                if (enhanced) {
                    if (line_num == line) {
                        std::printf("  > %4d | %s\n", line_num, text.c_str());
                    } else {
                        std::printf("    %4d | %s\n", line_num, text.c_str());
                    }
                } else {
                    std::printf("%6d | %s\n", line_num, text.c_str());
                }
            }

            if (enhanced) {
                int cx = r.value("enclosing_complexity", 0);
                int loc = r.value("enclosing_lines", 0);
                int incoming = r.value("enclosing_refs", 0);
                std::string metrics =
                    format_metrics_line_text(cx, loc, incoming);
                if (!metrics.empty()) {
                    std::printf("  metrics: %s\n", metrics.c_str());
                }
            }

            std::printf("\n");
        }

        (void)max_lines;
        return 0;
    }

    if (json_output) {
        // Match Go's `lci search --json` wire format faithfully:
        //   - Each result element is wrapped in `{"result": {...}}` (Go's
        //     `searchtypes.StandardResult` has a `Result GrepResult
        //     json:"result"` tag — the wrapper is part of the contract).
        //   - Paths are emitted relative to cwd (Go calls
        //     `pathutil.ToRelativeStandardResults(results, projectRoot)`).
        //   - Top-level `mode` is "standard" (Go's standard-results path
        //     adds `"mode": "standard"`; integrated-mode would override).
        std::error_code rel_ec;
        auto cwd = std::filesystem::current_path(rel_ec);
        auto raw_results = j.value("results", nlohmann::json::array());
        nlohmann::json wrapped = nlohmann::json::array();
        for (auto& r : raw_results) {
            std::string path = r.value("path", "");
            if (!rel_ec && !path.empty()) {
                std::error_code ec;
                auto rel = std::filesystem::relative(path, cwd, ec);
                if (!ec) {
                    r["path"] = rel.string();
                }
            }
            wrapped.push_back(nlohmann::json{{"result", r}});
        }
        nlohmann::json output;
        output["query"] = pattern;
        output["time_ms"] = elapsed_ms;
        output["count"] = wrapped.size();
        output["results"] = wrapped;
        output["mode"] = "standard";
        std::cout << output.dump(2) << "\n";
        return 0;
    }

    auto results = j.value("results", nlohmann::json::array());

    if (compact_search) {
        std::printf("Found %zu matches in %.1fms (compact mode)\n\n",
                    results.size(), elapsed_ms);
        // Resolve the current working directory once so we can render
        // paths relative to it, mirroring Go's compact-mode formatter
        // (`b.py:1:` rather than `/abs/path/to/b.py:1:`).
        for (auto& r : results) {
            std::string path = to_relative_display_path(r.value("path", ""));
            int line = r.value("line", 0);
            auto context = r.value("context", nlohmann::json::object());
            int start_line = context.value("start_line", 0);
            auto lines = context.value("lines", nlohmann::json::array());
            std::string display_path = path;

            for (size_t i = 0; i < lines.size(); ++i) {
                int line_num = start_line + static_cast<int>(i);
                if (line_num == line) {
                    // Lines from context.lines may carry a trailing
                    // '\n' (the indexer preserves the file's final
                    // newline on the last line). printf adds its own
                    // newline below, so strip the trailing one to
                    // avoid an extra blank line in compact output.
                    std::string text = lines[i].get<std::string>();
                    if (!text.empty() && text.back() == '\n') text.pop_back();
                    std::printf("%s:%d: %s\n", display_path.c_str(), line_num,
                                text.c_str());
                    break;
                }
            }
        }
        return 0;
    }

    std::printf("Found %zu results in %.1fms (standard mode)\n\n",
                results.size(), elapsed_ms);

    for (auto& r : results) {
        std::string path = to_relative_display_path(r.value("path", ""));
        int line = r.value("line", 0);
        auto context = r.value("context", nlohmann::json::object());
        std::string block_name = context.value("block_name", "");
        std::string block_type = context.value("block_type", "");
        int start_line = context.value("start_line", 0);
        auto lines = context.value("lines", nlohmann::json::array());

        if (case_insensitive && start_line == line - 1 && line > 1 &&
            !lines.empty()) {
            std::string first = lines[0].get<std::string>();
            if (!first.empty() && first.back() == '\n') first.pop_back();
            if (first.empty()) {
                auto prior_line =
                    read_line_from_file(r.value("path", ""), line - 2);
                if (!prior_line.empty()) {
                    lines.insert(lines.begin(), prior_line);
                    start_line = line - 2;
                }
            }
        }

        std::printf("%s:%d", path.c_str(), line);
        if (!block_name.empty()) {
            std::printf(" (in %s %s)", block_type.c_str(), block_name.c_str());
        }
        std::printf("\n");

        for (size_t i = 0; i < lines.size(); ++i) {
            int line_num = start_line + static_cast<int>(i);
            // Match Go's text-mode output exactly: a 6-char right-
            // padded line number + " | " + content + "\n". No '>'
            // marker on the match line — Go's formatter prints every
            // context line uniformly and parity is more valuable than
            // the marker. Strip any trailing '\n' on the source line
            // so printf's own newline doesn't double up.
            std::string text = lines[i].get<std::string>();
            if (!text.empty() && text.back() == '\n') text.pop_back();
            std::printf("%6d | %s\n", line_num, text.c_str());
        }
        // Go's standard-mode formatter prints two blank lines between
        // result blocks (one closing the context, one before the next
        // path). Match that spacing exactly.
        std::printf("\n\n");
    }

    (void)max_lines;
    return 0;
}

int run_grep(const GlobalFlags& flags, const std::string& pattern,
             int max_results, int context_lines, bool case_insensitive,
             bool json_output, const std::string& /*exclude_pattern*/,
             const std::string& /*include_pattern*/, bool exclude_tests,
             bool exclude_comments, bool /*use_regex*/,
             bool invert_match,
             const std::vector<std::string>& extra_patterns,
             bool count_per_file, bool files_only,
             int max_count_per_file) {
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

    std::string search_err;
    std::optional<nlohmann::json> result;
    if (all_patterns.size() == 1) {
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

    return 0;
}

}  // namespace cli
}  // namespace lci
