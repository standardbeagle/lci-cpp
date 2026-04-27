#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <lci/error.h>

namespace lci {

/// Snapshot of indexing progress returned by ProgressTracker::get_progress().
struct IndexingProgress {
    int files_processed{};
    int total_files{};
    std::string current_file;
    double files_per_second{};
    std::chrono::seconds estimated_time_left{};
    std::vector<Error> errors;
    double scanning_progress{};
    double indexing_progress{};
    bool is_scanning{};
    std::chrono::milliseconds elapsed{};
};

/// Thread-safe progress tracker for the indexing pipeline.
///
/// Uses atomic counters for the hot path (increment operations) and a
/// mutex only for the current-file string and error list.  The tracker
/// has two phases: scanning (file discovery) and indexing (processing).
class ProgressTracker {
  public:
    ProgressTracker();

    /// Sets the total file count and transitions from scanning to indexing.
    void set_total(int total);

    /// Increments the scanned-files counter during discovery.
    void increment_scanned();

    /// Increments the processed-files counter and updates the current file.
    void increment_processed(const std::string& current_file);

    /// Increments the integrated-files counter.
    void increment_integrated();

    /// Records an indexing error.
    void add_error(Error err);

    /// Returns a snapshot of current progress.
    IndexingProgress get_progress() const;

    /// Returns the number of integrated files.
    int integrated_count() const;

  private:
    static constexpr double kEstimatedScanningProgress = 50.0;

    std::chrono::steady_clock::time_point start_time_;

    std::atomic<int64_t> total_files_{0};
    std::atomic<int64_t> scanned_files_{0};
    std::atomic<int64_t> processed_files_{0};
    std::atomic<int64_t> integrated_files_{0};
    std::atomic<int32_t> is_scanning_{1};

    mutable std::mutex current_file_mu_;
    std::string current_file_;

    mutable std::mutex errors_mu_;
    std::vector<Error> errors_;
};

}  // namespace lci
