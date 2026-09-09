#include <lci/git/frequency_analyzer.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <lci/config/gitignore.h>
#include <lci/core/portable.h>

namespace lci {
namespace git {

namespace {

/// Returns current Unix epoch in seconds.
int64_t now_epoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Lowercase ASCII in-place.
void ascii_lower(std::string& s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
}

/// Glob match for churn include/exclude patterns. Case-insensitive; the
/// pattern is tried against the full path and against the base name.
///
/// Wildcard semantics are delegated to lci::glob_match (the gitignore
/// matcher) so this file does not carry a second, weaker dialect. The
/// hand-rolled predecessor silently never matched two whole pattern classes:
/// anything starting with "**/" (its prefix check required a non-empty
/// prefix before the stars) and anything using '?' or '[]' (it only ever
/// looked at '*').
bool matches_glob(std::string_view lower_path, std::string_view lower_base,
                  std::string_view pattern) {
    if (pattern.empty()) return false;

    // "vendor/*" style directory prefixes exclude the WHOLE subtree at any
    // depth, which `*` alone does not express (it stops at '/').
    if (pattern.ends_with("/*")) {
        auto prefix = pattern.substr(0, pattern.size() - 2);
        if (!prefix.empty()) {
            if (lower_path.size() > prefix.size() &&
                lower_path.compare(0, prefix.size(), prefix) == 0 &&
                lower_path[prefix.size()] == '/') {
                return true;
            }
            size_t pos = 0;
            while ((pos = lower_path.find(prefix, pos)) !=
                   std::string_view::npos) {
                size_t after = pos + prefix.size();
                if (pos > 0 && lower_path[pos - 1] == '/' &&
                    after < lower_path.size() && lower_path[after] == '/') {
                    return true;
                }
                ++pos;
            }
        }
    }

    // "**/" anchors nothing: strip it and match the remainder anywhere.
    if (pattern.starts_with("**/")) {
        return matches_glob(lower_path, lower_base, pattern.substr(3));
    }

    // "docs/**" style subtree patterns.
    if (auto star = pattern.find("**"); star != std::string_view::npos) {
        auto prefix = pattern.substr(0, star);
        if (!prefix.empty() && lower_path.starts_with(prefix)) return true;
    }

    if (lci::glob_match(pattern, lower_base)) return true;
    if (lci::glob_match(pattern, lower_path)) return true;

    // A pattern without a directory component matches at any depth.
    if (pattern.find('/') == std::string_view::npos) return false;
    for (size_t i = 0; i + 1 < lower_path.size(); ++i) {
        if (lower_path[i] == '/' &&
            lci::glob_match(pattern, lower_path.substr(i + 1))) {
            return true;
        }
    }
    return false;
}

/// Default patterns of files excluded from churn analysis.
const std::string_view kExcludedPatterns[] = {
    "CHANGELOG*", "HISTORY*", "CHANGES*", "NEWS*", "RELEASE*",
    "*.md", "*.rst", "*.txt", "docs/*", "doc/*", "documentation/*",
    "*.min.js", "*.min.css", "*.bundle.js", "*.bundle.css",
    "*.generated.*", "*.g.dart", "*.freezed.dart", "*.d.ts",
    "package-lock.json", "yarn.lock", "pnpm-lock.yaml",
    "Gemfile.lock", "poetry.lock", "Cargo.lock", "go.sum", "composer.lock",
    "dist/*", "build/*", "out/*", "target/*", ".next/*", "bin/*", "obj/*",
    "Debug/*", "Release/*", "x64/*", "x86/*", "artifacts/*", "output/*",
    "_build/*", "__pycache__/*", ".cache/*",
    "*.exe", "*.dll", "*.so", "*.dylib", "*.a", "*.lib", "*.o", "*.obj",
    "*.pyc", "*.pyo", "*.class", "*.jar", "*.war", "*.ear", "*.wasm",
    "*.bin", "*.dat", "*.db", "*.sqlite", "*.sqlite3",
    "*.png", "*.jpg", "*.jpeg", "*.gif", "*.ico", "*.svg", "*.webp",
    "*.bmp", "*.tiff", "*.mp3", "*.mp4", "*.wav", "*.avi", "*.mov",
    "*.webm", "*.ogg", "*.flac", "*.pdf",
    "*.woff", "*.woff2", "*.ttf", "*.otf", "*.eot",
    "*.zip", "*.tar", "*.gz", "*.tgz", "*.bz2", "*.xz", "*.7z", "*.rar",
    "*.nupkg", "*.gem", "*.egg", "*.whl",
    // "packages/*" is NOT excluded: it is the standard first-party layout
    // for pnpm/yarn/lerna monorepos, so blanket-excluding it hid the churn
    // of the very code the report exists to rank. Genuinely third-party
    // trees are already covered by node_modules / vendor / bower_components.
    "vendor/*", "node_modules/*", "third_party/*",
    "bower_components/*",
    ".idea/*", ".vscode/*", "*.iml", "*.suo", "*.user",
    ".github/*", ".gitlab-ci.yml", ".travis.yml", "Jenkinsfile",
    "coverage/*", ".nyc_output/*", "*.coverage", "*.lcov",
    "test-results/*", "junit.xml",
};

const std::string_view kExcludedExact[] = {
    "CHANGELOG.md", "CHANGELOG", "HISTORY.md", "CHANGES.md", "NEWS.md",
    "RELEASES.md", "package-lock.json", "yarn.lock", "pnpm-lock.yaml",
    "go.sum", "Cargo.lock", "poetry.lock", "composer.lock", "Gemfile.lock",
};

std::string file_base_name(std::string_view path) {
    auto sep = path.find_last_of("/\\");
    if (sep == std::string_view::npos) return std::string(path);
    return std::string(path.substr(sep + 1));
}

}  // namespace

// ============================================================================
// TimeWindow parsing
// ============================================================================

TimeWindow parse_time_window(std::string_view s) {
    if (s == "7d" || s == "7days" || s == "week") return TimeWindow::Days7;
    if (s == "30d" || s == "30days" || s == "month") return TimeWindow::Days30;
    if (s == "90d" || s == "90days" || s == "quarter") return TimeWindow::Days90;
    if (s == "1y" || s == "1year" || s == "year" || s == "365d") return TimeWindow::Year1;
    return TimeWindow::Days30;
}

// ============================================================================
// Volatility / Collision scoring
// ============================================================================

double calculate_volatility_score(int change_count, int lines_changed,
                                  int unique_authors, double window_days) {
    if (window_days <= 0.0) window_days = 30.0;

    double change_rate = static_cast<double>(change_count) / window_days;
    double norm_change = std::min(change_rate / 1.0, 1.0);

    double churn_rate = static_cast<double>(lines_changed) / window_days;
    double norm_churn = std::min(churn_rate / 100.0, 1.0);

    double norm_author = std::min(static_cast<double>(unique_authors) / 5.0, 1.0);

    return 0.4 * norm_change + 0.4 * norm_churn + 0.2 * norm_author;
}

double calculate_collision_score(
    const std::vector<ContributorActivity>& contributors,
    int recent_changes) {
    if (contributors.size() < 2) return 0.0;

    double author_factor =
        std::min(static_cast<double>(contributors.size() - 1) / 4.0, 1.0) * 0.4;
    double recency_factor =
        std::min(static_cast<double>(recent_changes) / 10.0, 1.0) * 0.4;

    if (contributors.size() >= 2 && contributors[0].ownership_share > 0.0) {
        double ratio = contributors[1].ownership_share /
                       contributors[0].ownership_share;
        return author_factor + recency_factor + ratio * 0.2;
    }
    return author_factor + recency_factor;
}

FindingSeverity determine_collision_severity(double score) {
    if (score >= 0.7) return FindingSeverity::Critical;
    if (score >= 0.4) return FindingSeverity::Warning;
    return FindingSeverity::Info;
}

// ============================================================================
// Churn filter
// ============================================================================

bool should_exclude_from_churn(std::string_view file_path) {
    return should_exclude_from_churn(file_path, {}, {}, false);
}

bool should_exclude_from_churn(std::string_view file_path,
                               const std::vector<std::string>& include_patterns,
                               const std::vector<std::string>& exclude_patterns,
                               bool skip_defaults) {
    std::string normalized(file_path);
    for (auto& c : normalized) {
        if (c == '\\') c = '/';
    }
    std::string lower_path = normalized;
    ascii_lower(lower_path);
    std::string base = file_base_name(file_path);
    std::string lower_base = base;
    ascii_lower(lower_base);

    // If include patterns are set, file MUST match at least one.
    if (!include_patterns.empty()) {
        bool matched = false;
        for (const auto& pat : include_patterns) {
            std::string lp = pat;
            ascii_lower(lp);
            if (matches_glob(lower_path, lower_base, lp)) {
                matched = true;
                break;
            }
        }
        if (!matched) return true;
    }

    // Custom exclude patterns.
    for (const auto& pat : exclude_patterns) {
        std::string lp = pat;
        ascii_lower(lp);
        if (matches_glob(lower_path, lower_base, lp)) return true;
    }

    if (skip_defaults) return false;

    // Exact matches.
    for (auto exact : kExcludedExact) {
        if (base == exact) return true;
    }

    // Default glob patterns.
    for (auto pattern : kExcludedPatterns) {
        std::string lp(pattern);
        ascii_lower(lp);
        if (matches_glob(lower_path, lower_base, lp)) return true;
    }

    return false;
}

// ============================================================================
// ChangeFrequencyParams
// ============================================================================

ChangeFrequencyParams ChangeFrequencyParams::defaults() {
    return {"30d", "file", {"all"}, {}, {}, {}, 2, 2, 50, {}, {}, false};
}

bool ChangeFrequencyParams::has_focus(FrequencyFocus f) const {
    if (focus.empty()) return true;
    std::string_view target;
    switch (f) {
        case FrequencyFocus::Hotspots: target = "hotspots"; break;
        case FrequencyFocus::Collisions: target = "collisions"; break;
        case FrequencyFocus::Patterns: target = "patterns"; break;
        case FrequencyFocus::Ownership: target = "ownership"; break;
        case FrequencyFocus::All: target = "all"; break;
    }
    for (const auto& s : focus) {
        if (s == target || s == "all") return true;
    }
    return false;
}

TimeWindow ChangeFrequencyParams::get_time_window() const {
    if (time_window.empty()) return TimeWindow::Days30;
    return parse_time_window(time_window);
}

FrequencyGranularity ChangeFrequencyParams::get_granularity() const {
    if (granularity == "symbol") return FrequencyGranularity::Symbol;
    return FrequencyGranularity::File;
}

// ============================================================================
// HistoryProvider
// ============================================================================

HistoryProvider::HistoryProvider(Provider& provider) : provider_(provider) {}

bool HistoryProvider::get_commit_history(int64_t since_epoch,
                                         std::vector<CommitInfo>& out,
                                         const std::vector<std::string>& paths) {
    // Format since as ISO date for git --since, in UTC WITH the trailing Z:
    // git parses a zone-less ISO date in LOCAL time, which slides the cutoff
    // by the host's UTC offset (up to +/-14h) and silently includes/excludes
    // a half-day of history.
    time_t t = static_cast<time_t>(since_epoch);
    struct tm tm_buf {};
    portable::gmtime_utc(t, tm_buf);
    char since_str[32];
    std::strftime(since_str, sizeof(since_str), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

    std::vector<std::string> args = {
        "log", "--numstat", "-z",
        "--format=%H%x00%an%x00%ae%x00%at%x00%s",
        std::string("--since=") + since_str,
        "--no-merges",
    };

    if (!paths.empty()) {
        args.emplace_back("--");
        for (const auto& p : paths) args.push_back(p);
    }

    // Reuse Provider::run_git — it argv-execs git in the repo root with no
    // shell at all (subprocess::run_capture). Fields are NUL-separated
    // (%x00) and -z makes numstat paths raw bytes, so author names or file
    // names containing '|', spaces, quotes, or non-ASCII bytes cannot shift
    // the parse the way a |-separated, core.quotePath-mangled stream did.
    std::string output;
    // A non-zero git exit means the history is incomplete (bad ref, broken
    // pipe, mid-stream failure). Fail rather than parse a truncated stream and
    // present partial change-frequency data as a complete result — the caller
    // cannot otherwise distinguish "few commits" from "git errored".
    if (!provider_.run_git(args, output)) return false;

    return parse_commit_history(output, out);
}

bool HistoryProvider::get_file_history(std::string_view file_path,
                                       int64_t since_epoch,
                                       std::vector<CommitInfo>& out) {
    return get_commit_history(since_epoch, out, {std::string(file_path)});
}

bool HistoryProvider::get_repo_history(int64_t since_epoch,
                                       std::string_view pattern,
                                       std::vector<CommitInfo>& out,
                                       std::string* notice) {
    if (pattern.empty()) {
        return get_commit_history(since_epoch, out);
    }

    // One bounded ls-files (a single pathspec arg) only to tell "pattern
    // matched nothing" apart from "no commits in window" — the two read
    // identically as an empty history. The pattern itself goes to `log` as
    // a native pathspec below: expanding matches into argv grew unbounded
    // with the repo (E2BIG past ARG_MAX; 32KB on Windows) and failed the
    // whole hotspots report.
    std::string listing;
    if (!provider_.run_git({"ls-files", "--", std::string(pattern)},
                           listing)) {
        // A failed ls-files used to fall back to FULL-repo history, which is
        // the opposite of what the caller asked for: a hotspot report scoped
        // to one pattern silently became a whole-repo report, with no way to
        // tell the two apart. Fail instead (Karpathy rule 6).
        return false;
    }

    if (listing.find_first_not_of(" \t\r\n") == std::string::npos) {
        // Zero matches is a legitimate answer, not a reason to widen the
        // scope. Return an empty history and say why.
        out.clear();
        if (notice) {
            *notice = "file_pattern \"" + std::string(pattern) +
                      "\" matched no tracked files";
        }
        return true;
    }
    // After "--", git treats each entry as a pathspec, with exactly the
    // matching semantics ls-files just used — one arg, any glob.
    return get_commit_history(since_epoch, out, {std::string(pattern)});
}

bool parse_commit_history(std::string_view output,
                          std::vector<CommitInfo>& out) {
    // Consumes `git log --numstat -z --format=%H%x00%an%x00%ae%x00%at%x00%s`.
    // Byte layout per commit (verified against git 2.43):
    //   hash \0 name \0 email \0 ts \0 subject \0
    //   then numstat entries, the first prefixed with a '\n':
    //     "A\tD\tpath" \0                 — add/modify/delete
    //     "A\tD\t" \0 old \0 new \0       — rename/copy (empty path field)
    // An empty commit has no entries and no '\n'; the next header follows
    // its subject NUL directly. NUL cannot appear in any field value, so
    // splitting on it is unambiguous — unlike the old |-separated format,
    // where a '|' in the author name shifted every later field.
    size_t pos = 0;
    auto next_field = [&](std::string_view& field) {
        if (pos >= output.size()) return false;
        size_t end = output.find('\0', pos);
        if (end == std::string_view::npos) end = output.size();
        field = output.substr(pos, end - pos);
        pos = end + 1;
        return true;
    };
    auto is_hash = [](std::string_view f) {
        if (f.size() != 40 && f.size() != 64) return false;
        for (char c : f) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }
        return true;
    };

    CommitInfo* current = nullptr;
    bool first_entry_of_commit = false;
    std::string_view field;
    while (next_field(field)) {
        if (first_entry_of_commit && !field.empty() && field.front() == '\n') {
            field.remove_prefix(1);
        }
        first_entry_of_commit = false;
        if (field.empty()) continue;

        if (is_hash(field)) {
            CommitInfo ci;
            ci.hash = std::string(field);
            std::string_view name, email, ts, subject;
            if (!next_field(name) || !next_field(email) || !next_field(ts) ||
                !next_field(subject)) {
                break;  // truncated stream: keep what parsed cleanly
            }
            ci.author_name = std::string(name);
            ci.author_email = std::string(email);
            std::from_chars(ts.data(), ts.data() + ts.size(),
                            ci.timestamp_epoch);
            ci.message = std::string(subject);
            out.push_back(std::move(ci));
            current = &out.back();
            first_entry_of_commit = true;
            continue;
        }

        // Numstat entry: added \t deleted \t path.
        if (current == nullptr) continue;
        auto tab1 = field.find('\t');
        if (tab1 == std::string_view::npos) continue;
        auto tab2 = field.find('\t', tab1 + 1);
        if (tab2 == std::string_view::npos) continue;

        int added = 0, deleted = 0;
        auto added_str = field.substr(0, tab1);
        auto deleted_str = field.substr(tab1 + 1, tab2 - tab1 - 1);
        auto path = field.substr(tab2 + 1);
        if (added_str != "-") {
            std::from_chars(added_str.data(),
                            added_str.data() + added_str.size(), added);
        }
        if (deleted_str != "-") {
            std::from_chars(deleted_str.data(),
                            deleted_str.data() + deleted_str.size(), deleted);
        }

        FileChange fc;
        if (path.empty()) {
            // Rename/copy: old and new paths are the next two NUL fields.
            std::string_view old_p, new_p;
            if (!next_field(old_p) || !next_field(new_p)) break;
            fc.old_path = std::string(old_p);
            fc.path = std::string(new_p);
        } else {
            fc.path = std::string(path);
        }
        fc.lines_added = added;
        fc.lines_deleted = deleted;
        fc.status = determine_change_status(added, deleted, fc.old_path);
        current->file_changes.push_back(std::move(fc));
    }

    return true;
}

void parse_rename_path(std::string_view path,
                                        std::string& new_path,
                                        std::string& old_path) {
    std::string ps(path);
    // Handle {prefix/old => prefix/new} format.
    auto brace = ps.find('{');
    if (brace != std::string::npos) {
        auto arrow = ps.find(" => ", brace);
        auto close = ps.find('}', brace);
        if (arrow != std::string::npos && close != std::string::npos) {
            auto prefix = ps.substr(0, brace);
            auto old_part = ps.substr(brace + 1, arrow - brace - 1);
            auto new_part = ps.substr(arrow + 4, close - arrow - 4);
            auto suffix = ps.substr(close + 1);
            new_path = prefix + new_part + suffix;
            old_path = prefix + old_part + suffix;
            return;
        }
    }

    // Simple "old => new" format.
    auto arrow = ps.find(" => ");
    if (arrow != std::string::npos) {
        old_path = ps.substr(0, arrow);
        new_path = ps.substr(arrow + 4);
        // Trim whitespace.
        while (!old_path.empty() && old_path.back() == ' ') old_path.pop_back();
        while (!new_path.empty() && new_path.front() == ' ') new_path.erase(0, 1);
        return;
    }

    new_path = ps;
    old_path.clear();
}

std::string determine_change_status(int added, int deleted,
                                              const std::string& old_path) {
    if (!old_path.empty()) return "R";
    if (added > 0 && deleted == 0) return "A";
    if (added == 0 && deleted > 0) return "D";
    return "M";
}

// ============================================================================
// FrequencyAnalyzer
// ============================================================================

FrequencyAnalyzer::FrequencyAnalyzer(Provider& provider)
    : history_(provider) {}

bool FrequencyAnalyzer::analyze(const ChangeFrequencyParams& params,
                                ChangeFrequencyReport& out) {
    auto start = std::chrono::steady_clock::now();

    int top_n = params.top_n > 0 ? params.top_n : 50;
    int min_changes = params.min_changes > 0 ? params.min_changes : 2;
    int min_contributors = params.min_contributors > 0 ? params.min_contributors : 2;

    TimeWindow window = params.get_time_window();
    int64_t window_secs = time_window_seconds(window);
    int64_t since = now_epoch() - window_secs;

    out = ChangeFrequencyReport{};
    out.metadata.analyzed_at_epoch = now_epoch();
    out.metadata.time_window = std::string(to_string(window));
    out.metadata.window_start_epoch = since;
    out.metadata.window_end_epoch = now_epoch();

    std::vector<CommitInfo> commits;
    bool ok;
    if (!params.file_path.empty()) {
        ok = history_.get_file_history(params.file_path, since, commits);
    } else if (!params.file_pattern.empty()) {
        ok = history_.get_repo_history(since, params.file_pattern, commits,
                                       &out.metadata.notice);
    } else {
        ok = history_.get_repo_history(since, "", commits);
    }
    if (!ok) return false;

    out.summary.total_commits_analyzed = static_cast<int>(commits.size());

    absl::flat_hash_map<std::string, FileChangeFrequency> file_stats;
    aggregate_by_file(commits, window, params.include_patterns,
                      params.exclude_patterns, params.skip_default_exclusions,
                      file_stats);
    out.summary.total_files_analyzed = static_cast<int>(file_stats.size());

    if (params.has_focus(FrequencyFocus::Hotspots)) {
        find_hotspots(file_stats, min_changes, top_n, out.hotspots);
        out.summary.hotspots_found = static_cast<int>(out.hotspots.size());
        if (!out.hotspots.empty()) {
            out.summary.highest_churn = out.hotspots[0].file_path;
        }
    }

    if (params.has_focus(FrequencyFocus::Collisions)) {
        find_collisions(file_stats, min_contributors, out.collisions);
        out.summary.collision_zones = static_cast<int>(out.collisions.size());
    }

    if (params.has_focus(FrequencyFocus::Ownership)) {
        calculate_ownership(file_stats, out.ownership);
    }

    out.summary.most_active_contributor = find_most_active_contributor(commits);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    out.metadata.compute_time_ms = elapsed;
    return true;
}

// ============================================================================
// Aggregation helpers
// ============================================================================

void FrequencyAnalyzer::aggregate_by_file(
    const std::vector<CommitInfo>& commits, TimeWindow window,
    const std::vector<std::string>& include_patterns,
    const std::vector<std::string>& exclude_patterns,
    bool skip_defaults,
    absl::flat_hash_map<std::string, FileChangeFrequency>& out) {

    double window_days = static_cast<double>(time_window_seconds(window)) / 86400.0;

    // First pass: aggregate file changes.
    for (const auto& commit : commits) {
        for (const auto& fc : commit.file_changes) {
            if (fc.path.empty()) continue;
            if (should_exclude_from_churn(fc.path, include_patterns,
                                          exclude_patterns, skip_defaults)) {
                continue;
            }

            auto& stats = out[fc.path];
            if (stats.file_path.empty()) {
                stats.file_path = fc.path;
                stats.metrics[window] = FrequencyMetrics{};
            }

            auto& metrics = stats.metrics[window];
            metrics.change_count++;
            metrics.lines_added += fc.lines_added;
            metrics.lines_deleted += fc.lines_deleted;

            if (metrics.first_change_epoch == 0 ||
                commit.timestamp_epoch < metrics.first_change_epoch) {
                metrics.first_change_epoch = commit.timestamp_epoch;
            }
            if (commit.timestamp_epoch > metrics.last_change_epoch) {
                metrics.last_change_epoch = commit.timestamp_epoch;
            }
        }

        // Track contributors per file.
        for (const auto& fc : commit.file_changes) {
            if (fc.path.empty()) continue;
            auto it = out.find(fc.path);
            if (it == out.end()) continue;

            auto& stats = it->second;
            bool found = false;
            for (auto& c : stats.contributors) {
                if (c.author_email == commit.author_email) {
                    c.change_count++;
                    c.lines_added += fc.lines_added;
                    c.lines_deleted += fc.lines_deleted;
                    if (commit.timestamp_epoch > c.last_change_epoch) {
                        c.last_change_epoch = commit.timestamp_epoch;
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                ContributorActivity ca;
                ca.author_name = commit.author_name;
                ca.author_email = commit.author_email;
                ca.change_count = 1;
                ca.lines_added = fc.lines_added;
                ca.lines_deleted = fc.lines_deleted;
                ca.last_change_epoch = commit.timestamp_epoch;
                stats.contributors.push_back(std::move(ca));
            }
        }
    }

    // Second pass: derived metrics.
    for (auto& [_, stats] : out) {
        auto& metrics = stats.metrics[window];
        metrics.unique_authors = static_cast<int>(stats.contributors.size());
        metrics.change_rate = static_cast<double>(metrics.change_count) / window_days;
        metrics.volatility_score = calculate_volatility_score(
            metrics.change_count, metrics.lines_added + metrics.lines_deleted,
            metrics.unique_authors, window_days);

        int total = metrics.change_count;
        for (auto& c : stats.contributors) {
            if (total > 0) {
                c.ownership_share =
                    static_cast<double>(c.change_count) / static_cast<double>(total);
            }
        }
        std::sort(stats.contributors.begin(), stats.contributors.end(),
                  [](const auto& a, const auto& b) {
                      if (a.change_count != b.change_count)
                          return a.change_count > b.change_count;
                      return a.author_email < b.author_email;
                  });
    }
}

void FrequencyAnalyzer::find_hotspots(
    const absl::flat_hash_map<std::string, FileChangeFrequency>& file_stats,
    int min_changes, int top_n,
    std::vector<FileChangeFrequency>& out) {

    for (const auto& [_, stats] : file_stats) {
        for (const auto& [__, metrics] : stats.metrics) {
            if (metrics.change_count >= min_changes) {
                out.push_back(stats);
            }
            break;  // Only check first window.
        }
    }

    // Total order: tied volatility breaks by path so the top_n cut keeps
    // the same files on every run regardless of hash iteration order.
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        double va = 0, vb = 0;
        for (const auto& [_, m] : a.metrics) { va = m.volatility_score; break; }
        for (const auto& [_, m] : b.metrics) { vb = m.volatility_score; break; }
        if (va != vb) return va > vb;
        return a.file_path < b.file_path;
    });

    if (static_cast<int>(out.size()) > top_n) {
        out.resize(static_cast<size_t>(top_n));
    }
}

void FrequencyAnalyzer::find_collisions(
    const absl::flat_hash_map<std::string, FileChangeFrequency>& file_stats,
    int min_contributors,
    std::vector<CollisionZone>& out) {

    int64_t cutoff = now_epoch() - 7 * 86400;

    for (const auto& [_, stats] : file_stats) {
        if (static_cast<int>(stats.contributors.size()) < min_contributors) continue;

        int recent = 0;
        for (const auto& c : stats.contributors) {
            if (c.last_change_epoch > cutoff) recent += c.change_count;
        }

        double score = calculate_collision_score(stats.contributors, recent);
        auto severity = determine_collision_severity(score);

        CollisionZone zone;
        zone.entity_type = "file";
        zone.path = stats.file_path;
        zone.contributors = stats.contributors;
        zone.collision_score = score;
        zone.severity = severity;
        zone.recent_changes = recent;
        zone.recommendation = generate_collision_recommendation(stats, severity);
        out.push_back(std::move(zone));
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.collision_score != b.collision_score)
            return a.collision_score > b.collision_score;
        return a.path < b.path;
    });
}

void FrequencyAnalyzer::calculate_ownership(
    const absl::flat_hash_map<std::string, FileChangeFrequency>& file_stats,
    std::vector<ModuleOwnership>& out) {

    absl::flat_hash_map<std::string, ModuleOwnership> modules;
    absl::flat_hash_map<std::string, absl::flat_hash_map<std::string, ContributorActivity>> mod_contribs;

    for (const auto& [path, stats] : file_stats) {
        auto mod = extract_module_path(path);
        if (mod.empty()) mod = ".";

        auto& m = modules[mod];
        m.module_path = mod;
        m.file_count++;
        for (const auto& [__, metrics] : stats.metrics) {
            m.total_changes += metrics.change_count;
            break;
        }

        for (const auto& c : stats.contributors) {
            auto& mc = mod_contribs[mod][c.author_email];
            if (mc.author_email.empty()) {
                mc.author_name = c.author_name;
                mc.author_email = c.author_email;
            }
            mc.change_count += c.change_count;
            mc.lines_added += c.lines_added;
            mc.lines_deleted += c.lines_deleted;
            if (c.last_change_epoch > mc.last_change_epoch) {
                mc.last_change_epoch = c.last_change_epoch;
            }
        }
    }

    for (auto& [mod_path, module] : modules) {
        auto& contribs = mod_contribs[mod_path];
        std::vector<ContributorActivity> list;
        list.reserve(contribs.size());
        for (auto& [_, ca] : contribs) {
            if (module.total_changes > 0) {
                ca.ownership_share = static_cast<double>(ca.change_count) /
                                     static_cast<double>(module.total_changes);
            }
            list.push_back(std::move(ca));
        }
        std::sort(list.begin(), list.end(),
                  [](const auto& a, const auto& b) {
                      if (a.change_count != b.change_count)
                          return a.change_count > b.change_count;
                      return a.author_email < b.author_email;
                  });

        if (!list.empty()) module.primary_owner = list[0];
        for (size_t i = 1; i < list.size(); ++i) {
            if (list[i].ownership_share >= 0.1) {
                module.secondary_owners.push_back(list[i]);
            }
        }

        out.push_back(std::move(module));
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.total_changes != b.total_changes)
            return a.total_changes > b.total_changes;
        return a.module_path < b.module_path;
    });
}

std::string find_most_active_contributor(
    const std::vector<CommitInfo>& commits) {
    absl::flat_hash_map<std::string, int> counts;
    absl::flat_hash_map<std::string, std::string> names;

    for (const auto& c : commits) {
        counts[c.author_email]++;
        names[c.author_email] = c.author_name;
    }

    int max_count = 0;
    std::string max_email;
    for (const auto& [email, count] : counts) {
        // Ties resolve to the lexicographically smallest email — never to
        // hash iteration order, which is salted per process.
        if (count > max_count || (count == max_count && email < max_email)) {
            max_count = count;
            max_email = email;
        }
    }

    if (auto it = names.find(max_email); it != names.end()) return it->second;
    return {};
}

std::string extract_module_path(std::string_view file_path) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start < file_path.size()) {
        auto sep = file_path.find_first_of("/\\", start);
        if (sep == std::string_view::npos) {
            parts.push_back(file_path.substr(start));
            break;
        }
        if (sep > start) parts.push_back(file_path.substr(start, sep - start));
        start = sep + 1;
    }

    if (parts.size() <= 1) return {};

    int depth = 2;
    if (static_cast<int>(parts.size()) - 1 < depth) {
        depth = static_cast<int>(parts.size()) - 1;
    }

    std::string result;
    for (int i = 0; i < depth; ++i) {
        if (i > 0) result += '/';
        result += std::string(parts[static_cast<size_t>(i)]);
    }
    return result;
}

std::string generate_collision_recommendation(
    const FileChangeFrequency& stats, FindingSeverity severity) {
    switch (severity) {
        case FindingSeverity::Critical:
            if (!stats.contributors.empty()) {
                auto share = static_cast<int>(stats.contributors[0].ownership_share * 100);
                return "High collision risk. Primary owner: " +
                       stats.contributors[0].author_name + " (" +
                       std::to_string(share) +
                       "%). Coordinate before making changes.";
            }
            return "High collision risk. Multiple developers actively editing.";
        case FindingSeverity::Warning:
            return "Moderate collision risk. Consider notifying recent contributors.";
        case FindingSeverity::Info:
            return "Low collision risk, but multiple contributors.";
    }
    return {};
}

}  // namespace git
}  // namespace lci
