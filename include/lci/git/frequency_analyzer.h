#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/git/pattern_detector.h>
#include <lci/git/provider.h>
#include <lci/git/types.h>

namespace lci {
namespace git {

// ============================================================================
// Time Window
// ============================================================================

/// Fixed analysis periods for change frequency.
enum class TimeWindow : uint8_t {
    Days7,
    Days30,
    Days90,
    Year1,
};

/// Returns the string representation (e.g. "7d", "30d").
constexpr std::string_view to_string(TimeWindow tw) {
    switch (tw) {
        case TimeWindow::Days7: return "7d";
        case TimeWindow::Days30: return "30d";
        case TimeWindow::Days90: return "90d";
        case TimeWindow::Year1: return "1y";
    }
    return "30d";
}

/// Converts a TimeWindow to a duration in seconds.
constexpr int64_t time_window_seconds(TimeWindow tw) {
    constexpr int64_t day = 86400;
    switch (tw) {
        case TimeWindow::Days7: return 7 * day;
        case TimeWindow::Days30: return 30 * day;
        case TimeWindow::Days90: return 90 * day;
        case TimeWindow::Year1: return 365 * day;
    }
    return 30 * day;
}

/// Parses a string to TimeWindow.
TimeWindow parse_time_window(std::string_view s);

// ============================================================================
// Frequency Granularity / Focus
// ============================================================================

enum class FrequencyGranularity : uint8_t { File, Symbol };
enum class FrequencyFocus : uint8_t { Hotspots, Collisions, Patterns, Ownership, All };

// ============================================================================
// Frequency Metrics
// ============================================================================

/// Change statistics for a file or symbol.
struct FrequencyMetrics {
    int change_count{};
    int lines_added{};
    int lines_deleted{};
    int unique_authors{};
    int64_t first_change_epoch{};
    int64_t last_change_epoch{};
    double change_rate{};
    double volatility_score{};
};

/// Computes a volatility score from metrics.
double calculate_volatility_score(int change_count, int lines_changed,
                                  int unique_authors, double window_days);

// ============================================================================
// Contributor / Collision
// ============================================================================

struct ContributorActivity {
    std::string author_name;
    std::string author_email;
    int change_count{};
    int lines_added{};
    int lines_deleted{};
    double ownership_share{};
    int64_t last_change_epoch{};
};

struct CollisionZone {
    std::string entity_type;
    std::string path;
    std::string symbol_name;
    std::vector<ContributorActivity> contributors;
    double collision_score{};
    FindingSeverity severity{FindingSeverity::Info};
    std::string recommendation;
    int recent_changes{};
};

double calculate_collision_score(const std::vector<ContributorActivity>& contributors,
                                 int recent_changes);

FindingSeverity determine_collision_severity(double score);

// ============================================================================
// File / Symbol Change Frequency
// ============================================================================

struct FileChangeFrequency {
    std::string file_path;
    absl::flat_hash_map<TimeWindow, FrequencyMetrics> metrics;
    std::vector<ContributorActivity> contributors;
    std::vector<AntiPattern> anti_patterns;
    int line_count{};
};

struct SymbolChangeFrequency {
    std::string symbol_name;
    std::string symbol_type;
    std::string file_path;
    int start_line{};
    int end_line{};
    absl::flat_hash_map<TimeWindow, FrequencyMetrics> metrics;
    std::vector<ContributorActivity> contributors;
};

// ============================================================================
// Module Ownership
// ============================================================================

struct ModuleOwnership {
    std::string module_path;
    ContributorActivity primary_owner;
    std::vector<ContributorActivity> secondary_owners;
    int total_changes{};
    int file_count{};
};

// ============================================================================
// Commit Info (parsed from git log)
// ============================================================================

struct FileChange {
    std::string path;
    std::string old_path;
    int lines_added{};
    int lines_deleted{};
    std::string status;
};

struct CommitInfo {
    std::string hash;
    std::string author_name;
    std::string author_email;
    int64_t timestamp_epoch{};
    std::string message;
    std::vector<FileChange> file_changes;
};

// ============================================================================
// Change Frequency Parameters / Report
// ============================================================================

struct ChangeFrequencyParams {
    std::string time_window;
    std::string granularity;
    std::vector<std::string> focus;
    std::string file_pattern;
    std::string file_path;
    std::string symbol_name;
    int min_changes{2};
    int min_contributors{2};
    int top_n{50};
    std::vector<std::string> include_patterns;
    std::vector<std::string> exclude_patterns;
    bool skip_default_exclusions{};

    static ChangeFrequencyParams defaults();
    bool has_focus(FrequencyFocus f) const;
    TimeWindow get_time_window() const;
    FrequencyGranularity get_granularity() const;
};

struct ChangeFrequencySummary {
    int total_files_analyzed{};
    int total_commits_analyzed{};
    int hotspots_found{};
    int collision_zones{};
    int anti_patterns_found{};
    std::string highest_churn;
    std::string most_active_contributor;
};

struct ChangeFrequencyMetadata {
    int64_t analyzed_at_epoch{};
    std::string time_window;
    int64_t window_start_epoch{};
    int64_t window_end_epoch{};
    std::string commit_range;
    int64_t compute_time_ms{};
    bool from_cache{};
};

struct ChangeFrequencyReport {
    ChangeFrequencySummary summary;
    std::vector<FileChangeFrequency> hotspots;
    std::vector<CollisionZone> collisions;
    std::vector<AntiPattern> anti_patterns;
    std::vector<SymbolChangeFrequency> symbol_details;
    std::vector<ModuleOwnership> ownership;
    ChangeFrequencyMetadata metadata;
};

// ============================================================================
// Churn Filter
// ============================================================================

/// Checks if a file should be excluded from churn analysis.
bool should_exclude_from_churn(std::string_view file_path);

bool should_exclude_from_churn(std::string_view file_path,
                               const std::vector<std::string>& include_patterns,
                               const std::vector<std::string>& exclude_patterns,
                               bool skip_defaults);

// ============================================================================
// Frequency Cache
// ============================================================================

/// Lightweight in-memory cache with TTL for frequency analysis results.
class FrequencyCache {
  public:
    explicit FrequencyCache(int64_t ttl_seconds = 600);

    /// Retrieves a cached file frequency, or returns false.
    bool get_file_frequency(std::string_view file_path, TimeWindow window,
                            FileChangeFrequency& out) const;

    /// Stores a file frequency result.
    void set_file_frequency(std::string_view file_path, TimeWindow window,
                            const FileChangeFrequency& freq);

    /// Retrieves a cached report, or returns false.
    bool get_report(std::string_view pattern, TimeWindow window,
                    ChangeFrequencyReport& out) const;

    /// Stores a full report.
    void set_report(std::string_view pattern, TimeWindow window,
                    const ChangeFrequencyReport& report);

    /// Removes all cached data for a file.
    void invalidate_file(std::string_view file_path);

    /// Removes all entries.
    void clear();

    struct Stats {
        uint64_t hits{};
        uint64_t misses{};
        double hit_rate{};
        int entry_count{};
    };

    Stats stats() const;

  private:
    struct Entry {
        std::variant<FileChangeFrequency, ChangeFrequencyReport> data;
        int64_t expires_at{};
        int64_t created_at{};
    };

    static std::string cache_key(std::string_view prefix, std::string_view path,
                                 TimeWindow window);

    mutable std::mutex mu_;
    absl::flat_hash_map<std::string, Entry> entries_;
    int64_t ttl_seconds_;
    int max_entries_{1000};
    mutable uint64_t hits_{};
    mutable uint64_t misses_{};

    void maybe_cleanup();
};

// ============================================================================
// Free utility functions (testable)
// ============================================================================

/// Parses git's rename notation into new and old paths.
void parse_rename_path(std::string_view path,
                       std::string& new_path, std::string& old_path);

/// Determines change status from stats.
std::string determine_change_status(int added, int deleted,
                                    const std::string& old_path);

/// Finds the most active contributor across commits.
std::string find_most_active_contributor(const std::vector<CommitInfo>& commits);

/// Extracts directory path up to 2 levels for module ownership.
std::string extract_module_path(std::string_view file_path);

/// Generates collision avoidance recommendation.
std::string generate_collision_recommendation(
    const FileChangeFrequency& stats, FindingSeverity severity);

// ============================================================================
// History Provider
// ============================================================================

/// Extends Provider with commit history queries for frequency analysis.
class HistoryProvider {
  public:
    explicit HistoryProvider(Provider& provider);

    /// Returns commits within a time window, optionally filtered by paths.
    bool get_commit_history(int64_t since_epoch, std::vector<CommitInfo>& out,
                            const std::vector<std::string>& paths = {});

    /// Returns commit history for a specific file.
    bool get_file_history(std::string_view file_path, int64_t since_epoch,
                          std::vector<CommitInfo>& out);

    /// Returns aggregate history for the entire repository.
    bool get_repo_history(int64_t since_epoch, std::string_view pattern,
                          std::vector<CommitInfo>& out);

  private:
    Provider& provider_;

    bool parse_commit_history(std::string_view output,
                              std::vector<CommitInfo>& out);
};

// ============================================================================
// Frequency Analyzer
// ============================================================================

/// Performs change frequency analysis on a git repository.
/// Ported from Go: internal/git/frequency_analyzer.go
class FrequencyAnalyzer {
  public:
    explicit FrequencyAnalyzer(Provider& provider);

    /// Runs a full frequency analysis with the given parameters.
    bool analyze(const ChangeFrequencyParams& params,
                 ChangeFrequencyReport& out);

    /// Analyzes a single file for the given time window.
    bool analyze_file(std::string_view file_path, TimeWindow window,
                      FileChangeFrequency& out);

    /// Checks collision risk for a specific file.
    bool get_collision_risk(std::string_view file_path, CollisionZone& out);

    /// Provides access to the cache for external invalidation.
    FrequencyCache& cache() { return cache_; }

  private:
    HistoryProvider history_;
    FrequencyCache cache_;

    void aggregate_by_file(
        const std::vector<CommitInfo>& commits, TimeWindow window,
        const std::vector<std::string>& include_patterns,
        const std::vector<std::string>& exclude_patterns,
        bool skip_defaults,
        absl::flat_hash_map<std::string, FileChangeFrequency>& out);

    void find_hotspots(
        const absl::flat_hash_map<std::string, FileChangeFrequency>& file_stats,
        int min_changes, int top_n,
        std::vector<FileChangeFrequency>& out);

    void find_collisions(
        const absl::flat_hash_map<std::string, FileChangeFrequency>& file_stats,
        int min_contributors,
        std::vector<CollisionZone>& out);

    void calculate_ownership(
        const absl::flat_hash_map<std::string, FileChangeFrequency>& file_stats,
        std::vector<ModuleOwnership>& out);
};

}  // namespace git
}  // namespace lci
