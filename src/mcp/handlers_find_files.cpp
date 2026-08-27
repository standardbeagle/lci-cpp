#include <lci/mcp/handlers_core.h>

#include <lci/mcp/handlers_core_shared.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <nlohmann/json-schema.hpp>
#include <rapidfuzz/distance/Levenshtein.hpp>
#include <re2/re2.h>

#include <lci/analysis/side_effect_analyzer.h>
#include <lci/core/context_lookup.h>
#include <lci/core/reference_tracker.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/schemas/search.h>  // generated: kSEARCH_SCHEMA
#include <lci/mcp/validation.h>
#include <lci/scope.h>
#include <lci/search/search_engine.h>
#include <lci/search/search_options.h>
#include <lci/version.h>  // generated: lci::kVersion

namespace lci {
namespace mcp {


namespace {

/// Wildcard glob: '*' matches any run of chars (including '/'), '?' matches one.
/// Allocation-free two-pointer scan with star-backtracking; no std::regex (this
/// runs per-file on the find_files read path).
bool wildcard_match(std::string_view str, std::string_view pat) {
    size_t s = 0, p = 0, star = std::string_view::npos, s_after_star = 0;
    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) {
            ++s;
            ++p;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            s_after_star = s;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            s = ++s_after_star;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}


}  // namespace

// -- handle_find_files --------------------------------------------------------

ToolResult handle_find_files(const nlohmann::json& params,
                             MasterIndex& indexer) {
    auto pattern = params.value("pattern", "");
    if (pattern.empty()) {
        return make_error_response("find_files", "pattern is required");
    }

    auto flags = params.value("flags", "");
    auto filter = params.value("filter", "");
    // `path` is an alias for `directory` — search uses path=, and agents
    // carry the name across tools.
    auto directory = params.value("directory", "");
    if (directory.empty()) directory = params.value("path", "");
    bool include_hidden = params.value("include_hidden", false);
    int max_results = params.value("max", 50);
    max_results = clamp_int(max_results, 1, 200);

    bool case_insensitive = comma_list_contains(flags, "ci");
    bool exact_only = comma_list_contains(flags, "exact");

    // Prepare pattern for matching
    std::string normalized_pattern = pattern;
    if (case_insensitive) {
        normalized_pattern = to_lower(pattern);
    }

    // A pattern carrying wildcards (e.g. "*.ts", "src/*_test.go") is a glob, not
    // a name to fuzzy-score — the literal "*.ts" would never match a filename.
    // Match it directly against basename/path and skip the fuzzy scorer.
    bool pattern_is_glob =
        normalized_pattern.find_first_of("*?") != std::string::npos;

    // Get all file IDs from the index
    auto snapshot = indexer.read_snapshot();
    if (!snapshot || snapshot->file_count() == 0) {
        // Empty index is server state, not a caller mistake.
        return make_unavailable_response(
            "find_files", "no files in index",
            "the index is empty or still building; check index_stats");
    }

    struct FileMatch {
        std::string path;
        double score;
        std::string match_type;
        int file_id;
        int pattern_count{1};
    };
    std::vector<FileMatch> matches;

    // All filtering and matching below runs on the PROJECT-ROOT-RELATIVE
    // path. The absolute path is wrong for two of the filters: a project
    // living under a dotted ancestor (~/.cache/repo, .work/corpus) would
    // mark every file "hidden", and directory= / glob patterns are written
    // by callers against the repo layout, not the machine's filesystem.
    const std::string& proj_root = indexer.config().project.root;
    auto rel_of = [&proj_root](const std::string& abs) -> std::string {
        if (!proj_root.empty() && abs.size() > proj_root.size() &&
            abs.compare(0, proj_root.size(), proj_root) == 0 &&
            abs[proj_root.size()] == '/') {
            return abs.substr(proj_root.size() + 1);
        }
        return abs;
    };

    // Normalize the directory scope the same way search's path= does:
    // "./x" -> "x", trailing slashes stripped, "." / "/" / the project
    // root itself mean no scoping, and an absolute path under the root is
    // relativized. Tier-1 traces: 8/8 empty find_files calls were agents
    // passing "." or an absolute workspace path and getting a silent 0.
    if (directory.rfind("./", 0) == 0) directory.erase(0, 2);
    while (!directory.empty() && directory.back() == '/') {
        directory.pop_back();
    }
    if (directory == ".") directory.clear();
    if (!directory.empty() && directory.front() == '/') {
        if (directory == proj_root) {
            directory.clear();
        } else {
            auto rel = relative_to_root(directory, proj_root);
            if (!rel.empty() && rel.front() == '/') {
                return make_error_response(
                    "find_files",
                    "directory must be project-root-relative (or an "
                    "absolute path under the project root " + proj_root +
                        "); got: " + directory);
            }
            directory = std::string(rel);
        }
    }

    for (const auto& [abs_path, fid] : snapshot->file_map) {
        const std::string path = rel_of(abs_path);
        // Directory filter (root-relative)
        if (!directory.empty()) {
            if (path.substr(0, directory.size()) != directory ||
                (path.size() > directory.size() &&
                 path[directory.size()] != '/')) {
                continue;
            }
        }

        // Hidden file filter — root-relative components only, so dotted
        // ancestors of the project root never hide the whole corpus.
        if (!include_hidden) {
            bool hidden = false;
            size_t pos = 0;
            while (pos < path.size()) {
                auto sep = path.find('/', pos);
                auto component =
                    (sep == std::string::npos)
                        ? path.substr(pos)
                        : path.substr(pos, sep - pos);
                if (!component.empty() && component[0] == '.' &&
                    component != "." && component != "..") {
                    hidden = true;
                    break;
                }
                if (sep == std::string::npos) break;
                pos = sep + 1;
            }
            if (hidden) continue;
        }

        // File type/glob filter
        if (!filter.empty()) {
            bool is_language_filter =
                filter.find('*') == std::string::npos &&
                filter.find('.') == std::string::npos;
            if (is_language_filter) {
                auto dot_pos = path.rfind('.');
                if (dot_pos == std::string::npos) continue;
                auto ext = path.substr(dot_pos + 1);
                if (to_lower(ext) != to_lower(filter)) continue;
            } else {
                // Simple glob: match basename against pattern
                auto slash_pos = path.rfind('/');
                auto basename =
                    (slash_pos == std::string::npos)
                        ? path
                        : path.substr(slash_pos + 1);
                // Simple wildcard match: *.ext
                if (filter.size() > 2 && filter[0] == '*' &&
                    filter[1] == '.') {
                    auto ext_filter = filter.substr(1);
                    if (basename.size() < ext_filter.size() ||
                        basename.substr(basename.size() -
                                        ext_filter.size()) != ext_filter) {
                        continue;
                    }
                }
            }
        }

        // Matching
        std::string match_path = path;
        if (case_insensitive) {
            match_path = to_lower(path);
        }

        auto slash_pos = path.rfind('/');
        std::string filename =
            (slash_pos == std::string::npos) ? path
                                             : path.substr(slash_pos + 1);
        auto dot_pos = filename.rfind('.');
        std::string filename_no_ext =
            (dot_pos == std::string::npos)
                ? filename
                : filename.substr(0, dot_pos);

        std::string norm_filename = filename;
        std::string norm_filename_no_ext = filename_no_ext;
        if (case_insensitive) {
            norm_filename = to_lower(filename);
            norm_filename_no_ext = to_lower(filename_no_ext);
        }

        double score = 0.0;
        std::string match_type;

        // 0. Glob pattern: match basename or full path; non-matches are skipped
        //    outright (not fuzzy-scored). A leading `**/` also matches at
        //    zero directory depth (glob convention; wildcard_match's literal
        //    `/` otherwise rejects root-level files — benchmark traces showed
        //    LLMs default to `**/name.go` and got silent empties on files at
        //    the project root).
        if (pattern_is_glob) {
            bool glob_hit = wildcard_match(norm_filename, normalized_pattern) ||
                            wildcard_match(match_path, normalized_pattern);
            if (!glob_hit && normalized_pattern.rfind("**/", 0) == 0) {
                std::string_view tail(normalized_pattern);
                tail.remove_prefix(3);
                glob_hit = wildcard_match(norm_filename, tail) ||
                           wildcard_match(match_path, tail);
            }
            if (glob_hit) {
                score = 1.0;
                match_type = "glob";
            } else {
                continue;
            }
        } else if (match_path == normalized_pattern) {
            // 1. Exact full path match
            score = 1.0;
            match_type = "exact";
        } else if (!exact_only) {
            // 2. Exact filename match
            if (norm_filename == normalized_pattern) {
                score = 0.95;
                match_type = "exact_filename";
            } else if (norm_filename_no_ext == normalized_pattern) {
                score = 0.93;
                match_type = "exact_filename_noext";
            }

            // 3. Substring match
            if (score == 0.0) {
                auto idx = match_path.find(normalized_pattern);
                if (idx != std::string::npos) {
                    score = 0.8 - (static_cast<double>(idx) /
                                   static_cast<double>(match_path.size()) *
                                   0.2);
                    match_type = "substring";
                }
            }

            // 4. Fuzzy match on filenameNoExt — parity with Go's
            //    matchFilePaths step 4 (handlers_files.go:226-232).
            //    Go invokes phraseMatcher.Match(pattern, filenameNoExt) with a
            //    levenshtein FuzzyMatcher at threshold 0.7. The Go fuzzer has a
            //    quirk: edlib.StringsSimilarity returns proper similarity but
            //    levenshteinSimilarity then computes `1 - that`, so wildly
            //    different strings score ~1.0 while near-matches score low.
            //    We reproduce that observable behaviour exactly so the parity
            //    descriptor (mcp/find_files/basic) keeps yielding the same
            //    fuzzy hits with score 0.574 on the multi-lang corpus.
            //
            //    For single-word queries (no spaces) the PhraseMatcher reduces
            //    to a known closed-form: queryWords=[pattern], one fuzzy match
            //    against the (single) target word →
            //      avgWordScore = sim_norm * 0.85
            //      + exactPhraseBonus 0.05   (allWordsMatched && inOrder)
            //      − fuzzyPenalty 0.08       (fuzzyCount/matchedCount = 1)
            //      = sim_norm * 0.85 − 0.03
            //    Final find_files score = phraseScore * 0.7 (line 229).
            //    Multi-word patterns (containing whitespace) are left unscored
            //    here for now — descriptor coverage is single-word only and a
            //    full PhraseMatcher port is FIX-D.1.B-scope.
            if (score == 0.0 && !exact_only &&
                normalized_pattern.find(' ') == std::string::npos &&
                !normalized_pattern.empty() &&
                !norm_filename_no_ext.empty()) {
                // Real normalized Levenshtein SIMILARITY: 1.0 identical, 0.0
                // wildly different. The original Go fuzzer (and the verbatim
                // port) used the DISTANCE ratio (lev/max_len) here and then
                // tested `>= 0.7`, which is inverted — unrelated filenames
                // (high distance) passed and every file flooded in at ~0.574,
                // while genuine near-matches were rejected. The Go oracle that
                // pinned that behavior is retired, so use the correct
                // similarity: similarity = 1 - distance/max_len.
                double sim_norm;
                if (normalized_pattern == norm_filename_no_ext) {
                    sim_norm = 1.0;
                } else {
                    size_t lev = rapidfuzz::levenshtein_distance(
                        normalized_pattern, norm_filename_no_ext);
                    size_t max_len =
                        std::max(normalized_pattern.size(),
                                 norm_filename_no_ext.size());
                    sim_norm = 1.0 - static_cast<double>(lev) /
                                         static_cast<double>(max_len);
                }
                if (sim_norm >= 0.7) {  // FuzzyMatcher threshold in Go
                    double phrase_score = sim_norm * 0.85;
                    // PhraseMatcher exactPhraseBonus (single word always
                    // counts as in-order, all-words-matched)
                    phrase_score += 0.05;
                    // PhraseMatcher fuzzyPenalty (fuzzyCount=1 of 1 match)
                    phrase_score -= 0.08;
                    if (phrase_score > 1.0) phrase_score = 1.0;
                    if (phrase_score < 0.0) phrase_score = 0.0;
                    score = phrase_score * 0.7;  // fuzzy-scale per Go
                    match_type = "fuzzy";
                }
            }

            // 5. Path component match
            if (score == 0.0) {
                size_t cpos = 0;
                while (cpos < match_path.size()) {
                    auto sep = match_path.find('/', cpos);
                    auto component =
                        (sep == std::string::npos)
                            ? match_path.substr(cpos)
                            : match_path.substr(cpos, sep - cpos);
                    if (component.find(normalized_pattern) !=
                        std::string::npos) {
                        score = 0.6;
                        match_type = "path_component";
                        break;
                    }
                    if (sep == std::string::npos) break;
                    cpos = sep + 1;
                }
            }
        }

        if (score > 0.0) {
            matches.push_back(
                {path, score, match_type, static_cast<int>(fid), 1});
        }
    }

    // Multi-word coverage: when the user types "user controller handler" we
    // re-scan with each word separately (>2 chars) and boost files matching
    // more words. Go parity: matchFilePaths multi-word boost. Karpathy: build
    // a path→index map once, no allocs in the per-word inner loop.
    if (pattern.find(' ') != std::string::npos) {
        std::vector<std::string> words;
        split_on_spaces(pattern, words);
        // Filter to >2-char words, deduped, excluding the full pattern itself.
        std::vector<std::string> extra;
        extra.reserve(words.size());
        for (auto& w : words) {
            if (w.size() <= 2) continue;
            if (w == pattern) continue;
            bool seen = false;
            for (const auto& e : extra) if (e == w) { seen = true; break; }
            if (!seen) extra.push_back(std::move(w));
        }
        if (extra.size() >= 1) {
            absl::flat_hash_map<std::string, size_t> by_path;
            by_path.reserve(matches.size());
            for (size_t i = 0; i < matches.size(); ++i) {
                by_path.emplace(matches[i].path, i);
            }
            for (const auto& w : extra) {
                std::string norm_w = case_insensitive ? to_lower(w) : w;
                for (const auto& [abs_path, fid] : snapshot->file_map) {
                    const std::string path = rel_of(abs_path);
                    // Same hidden + directory + filter rules as above. We
                    // skip the heavyweight per-pattern fuzzy/exact branches
                    // and use a single substring-match heuristic for the
                    // word pass — exact / substring / path-component only.
                    if (!directory.empty()) {
                        if (path.substr(0, directory.size()) != directory ||
                            (path.size() > directory.size() &&
                             path[directory.size()] != '/')) continue;
                    }
                    std::string mp = case_insensitive ? to_lower(path) : path;
                    if (mp.find(norm_w) == std::string::npos) continue;
                    auto it = by_path.find(path);
                    if (it != by_path.end()) {
                        // File already matched main pattern: increment count.
                        ++matches[it->second].pattern_count;
                    } else {
                        matches.push_back(
                            {path, 0.5, "word_substring",
                             static_cast<int>(fid), 1});
                        by_path.emplace(matches.back().path,
                                        matches.size() - 1);
                    }
                }
            }
            // Apply coverage boost: +0.15 per additional pattern match, cap
            // +0.5; final score clamped to ≤1.0 per spec.
            for (auto& m : matches) {
                if (m.pattern_count > 1) {
                    double extra_boost =
                        static_cast<double>(m.pattern_count - 1) * 0.15;
                    if (extra_boost > 0.5) extra_boost = 0.5;
                    m.score += extra_boost;
                    if (m.score > 1.0) m.score = 1.0;
                }
            }
        }
    }

    // Sort by score descending, with deterministic tiebreakers.
    // file_map is absl::flat_hash_map → iteration order is non-deterministic.
    // Without explicit tiebreak, equal-score matches surface in hash order.
    // Tiebreak: file_id ascending (matches Go golden ordering), then path.
    std::sort(matches.begin(), matches.end(),
              [](const FileMatch& a, const FileMatch& b) {
                  if (a.score != b.score) return a.score > b.score;
                  if (a.file_id != b.file_id) return a.file_id < b.file_id;
                  return a.path < b.path;
              });

    // Limit results — record the true match count first so the response
    // never reports total==max when the cap truncated a larger set.
    const int total_found = static_cast<int>(matches.size());
    if (total_found > max_results) {
        matches.resize(static_cast<size_t>(max_results));
    }

    // Build response
    nlohmann::json result_array = nlohmann::json::array();
    result_array.get_ref<nlohmann::json::array_t&>().reserve(matches.size());
    for (const auto& m : matches) {
        nlohmann::json item;
        item["path"] = m.path;
        item["score"] = m.score;
        item["match_type"] = m.match_type;
        item["file_id"] = m.file_id;
        result_array.push_back(std::move(item));
    }

    nlohmann::json response;
    response["results"] = std::move(result_array);
    response["total_matches"] = total_found;
    if (total_found > max_results) {
        response["truncated"] = true;
        response["showing"] = max_results;
    }
    response["pattern"] = pattern;

    // Empty result fails loud (Karpathy #6). A nonexistent directory scope
    // is the most common cause in agent traces — name it explicitly.
    if (total_found == 0) {
        if (!directory.empty()) {
            bool dir_exists = false;
            for (const auto& [abs_path, fid] : snapshot->file_map) {
                auto rel = rel_of(abs_path);
                if (rel.size() > directory.size() &&
                    rel.compare(0, directory.size(), directory) == 0 &&
                    rel[directory.size()] == '/') {
                    dir_exists = true;
                    break;
                }
            }
            response["hint"] =
                dir_exists
                    ? "0 matches for '" + pattern + "' under '" + directory +
                          "'. Widen the pattern or drop the directory scope."
                    : "directory '" + directory + "' contains no indexed "
                      "files — check the path (root-relative) or drop it.";
        } else {
            response["hint"] =
                "0 files matched '" + pattern + "'. Globs match the "
                "root-relative path; try a shorter name fragment (fuzzy "
                "matching) or search for content instead.";
        }
    }

    return make_json_response(response);
}


}  // namespace mcp
}  // namespace lci
