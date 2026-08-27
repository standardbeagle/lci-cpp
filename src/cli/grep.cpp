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
#include "search_shared.h"

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

using namespace grep_filters;


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

int run_grep(const GlobalFlags& flags, const GrepCommandOptions& options) {
    // Local aliases keep the (long) body identical to the flat-parameter era.
    const std::string& pattern = options.pattern;
    const std::vector<std::string>& paths = options.paths;
    const int max_results = options.max_results;
    const int context_lines = options.context_lines;
    const bool case_insensitive = options.case_insensitive;
    const bool json_output = options.json_output;
    const std::string& exclude_pattern = options.exclude_pattern;
    const std::string& include_pattern = options.include_pattern;
    const bool exclude_tests = options.exclude_tests;
    const bool exclude_comments = options.exclude_comments;
    const bool use_regex = options.use_regex;
    const bool invert_match = options.invert_match;
    const std::vector<std::string>& extra_patterns = options.extra_patterns;
    const bool count_per_file = options.count_per_file;
    const bool files_only = options.files_only;
    const int max_count_per_file = options.max_count_per_file;
    const bool verbose = options.verbose;

    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    // Resolve trailing path positionals to root-relative form (blocker: an
    // absolute or cwd-relative arg never matches the root-relative indexed
    // paths otherwise). Unknown/unindexed paths fail loudly server-side.
    std::error_code scope_cwd_ec;
    std::vector<std::string> scoped_paths = resolve_scope_paths(
        paths, cfg.project.root,
        std::filesystem::current_path(scope_cwd_ec).string());

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
            for (auto& seed : regex_literal_seeds(all_patterns[i])) {
                if (std::find(grep_seeds.begin(), grep_seeds.end(), seed) ==
                    grep_seeds.end()) {
                    grep_seeds.push_back(std::move(seed));
                }
            }
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
        // Search by literal seed(s), then RE2-filter the row set. The seed
        // search must OVER-collect: capping it at the user's -n spends the
        // whole budget on rows the regex then rejects (a 5-row page of
        // "handle_" hits rarely contains a "handle_\w+_context" line), so
        // small -n values returned empty for patterns with common seeds.
        // Page floor 1000; large explicit -n values raise it (bounded by
        // the server's 100k ceiling) so exhaustive extraction is not
        // silently truncated to the first page.
        result = search_union_patterns(
            *client, grep_seeds,
            std::min(100000, std::max(1000, max_results)), case_insensitive,
            search_err, scoped_paths);
    } else if (all_patterns.size() == 1) {
        result = client->search(all_patterns.front(), max_results,
                                case_insensitive, false, search_err,
                                scoped_paths);
    } else {
        result = search_union_patterns(*client, all_patterns, max_results,
                                       case_insensitive, search_err,
                                       scoped_paths);
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
    // so the downstream pipeline sees the regex-matching set. Truncate to
    // the user's -n only AFTER filtering (the seed search over-collected).
    if (grep_regex_filter) {
        auto raw = j.value("results", nlohmann::json::array());
        auto filtered_rows = regex_filter_results(std::move(raw),
                                                  *grep_regex_filter);
        if (max_results > 0 &&
            static_cast<int>(filtered_rows.size()) > max_results) {
            nlohmann::json capped = nlohmann::json::array();
            for (int i = 0; i < max_results; ++i) {
                capped.push_back(std::move(filtered_rows[i]));
            }
            filtered_rows = std::move(capped);
        }
        j["results"] = filtered_rows;
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
