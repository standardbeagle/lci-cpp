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
using namespace ast_filters;

std::optional<nlohmann::json> search_union_patterns(
    Client& client, const std::vector<std::string>& patterns, int max_results,
    bool case_insensitive, std::string& error,
    const std::vector<std::string>& paths) {
    nlohmann::json all = nlohmann::json::array();
    std::set<std::pair<std::string, int>> seen;
    for (const auto& p : patterns) {
        if (p.empty()) continue;
        std::string err;
        auto j = client.search(p, max_results, case_insensitive, false, err,
                               paths);
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

// Resolve the trailing path positionals (`lci grep pattern <path>...`, ripgrep
// `rg pattern [path...]` convention) to ROOT-relative form so they line up with
// the root-relative paths the index stores and matches against server-side.
//
// The indexed paths inside execute_search's `rel_in_scope` matcher are
// root-relative. A raw argv token is either absolute or relative to the user's
// current working directory, which need not equal the indexed root — so
// `lci grep pattern /abs/path/to/file` or a cwd-relative arg run from a
// subdirectory never prefix-matches a root-relative indexed path and silently
// matches nothing. Resolving here fixes that.
//
// Purely lexical (mirrors the std::filesystem::relative idiom used in
// debug.cpp's snapshot keying, but lexical so it needs no disk access and stays
// deterministic under tests). A token that cannot be expressed relative to
// `root` (it escapes the indexed root) is left unchanged; the server-side index
// membership check then reports it loudly as an unindexed path rather than
// silently returning empty. Fail-fast for unknown paths is enforced there — a
// filesystem::exists check here would pass files that exist on disk but were
// never indexed, which is the exact silent-empty bug this replaces.
namespace {


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

    // Resolve trailing path positionals to root-relative form (blocker: an
    // absolute or cwd-relative arg never matches the root-relative indexed
    // paths otherwise). Unknown/unindexed paths fail loudly server-side.
    std::error_code scope_cwd_ec;
    std::vector<std::string> scoped_paths = resolve_scope_paths(
        options.paths, cfg.project.root,
        std::filesystem::current_path(scope_cwd_ec).string());

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
    std::vector<std::string> regex_seeds;
    if (use_regex) {
        regex_seeds = regex_literal_seeds(effective_pattern);
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
        if (!regex_seeds.empty()) {
            // Fast path: seed-then-filter via the indexed server search.
            // ALL literal runs seed (union) — a single longest-run seed
            // was alternation-blind and silently dropped every branch
            // that lacked it.
            effective_pattern = regex_seeds.front();
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
        // Budget-exempt: this path mmaps files one at a time, so memory is
        // bounded regardless of corpus size, and grep must see every file.
        auto tasks = scanner.scan(/*apply_budget=*/false).tasks;
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
    if (use_regex && !regex_seeds.empty()) {
        all_patterns = regex_seeds;  // Union of every literal run.
    } else if (!effective_pattern.empty()) {
        // Bare top-level literal alternation ("FileWatcher|DebouncedRebuilder"
        // with no --regex): OR the branches through the literal fast path.
        // The literal engine would otherwise search for the pipe byte
        // verbatim and return a silent zero (Karpathy rule 6 class).
        auto or_terms = split_literal_alternation(effective_pattern);
        if (!or_terms.empty()) {
            if (!json_output) {
                std::cerr << "Note: pattern treated as OR of "
                          << or_terms.size()
                          << " literal terms (use --regex for full regex "
                             "syntax).\n";
            }
            all_patterns = std::move(or_terms);
        } else {
            all_patterns.push_back(effective_pattern);
        }
    }
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
                                false, search_err, scoped_paths);
    } else {
        result = search_union_patterns(*client, all_patterns, 500,
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

    // -- --group: per-file grouped output ------------------------------------
    //
    // Path once, total count, per-term counts (meaningful for OR queries:
    // bare `A|B` alternation or --patterns), sorted line numbers. Post-filter
    // over the already-capped row set — not a hot path.
    if (options.group) {
        auto rows = j.value("results", nlohmann::json::array());
        auto groups = group_rows_by_file(rows, all_patterns, case_insensitive);
        if (json_output) {
            nlohmann::json output{
                {"query", options.pattern},
                {"time_ms", elapsed_ms},
                {"count", rows.size()},
                {"unique_files", groups.size()},
                {"results", groups},
                {"mode", "group"},
            };
            std::cout << output.dump(2) << '\n';
            return 0;
        }
        std::printf("Found %zu matches in %zu file(s) in %.1fms (group mode)\n\n",
                    rows.size(), groups.size(), elapsed_ms);
        for (const auto& g : groups) {
            std::string path =
                to_relative_display_path(g.value("path", std::string{}));
            std::printf("%s: %d", path.c_str(), g.value("count", 0));
            const auto& terms = g["terms"];
            if (terms.is_object() && !terms.empty()) {
                std::printf(" (");
                bool first = true;
                for (auto it = terms.begin(); it != terms.end(); ++it) {
                    std::printf("%s%s x%d", first ? "" : ", ",
                                it.key().c_str(), it.value().get<int>());
                    first = false;
                }
                std::printf(")");
            }
            std::printf("  lines:");
            for (const auto& ln : g["lines"]) {
                std::printf(" %d", ln.get<int>());
            }
            std::printf("\n");
        }
        return 0;
    }

    return render_search_output(options, j, elapsed_ms, verbose_start);
}

}  // namespace cli
}  // namespace lci
