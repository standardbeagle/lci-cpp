#include <lci/indexing/pipeline_progress.h>

namespace lci {

ProgressTracker::ProgressTracker()
    : start_time_(std::chrono::steady_clock::now()) {}

void ProgressTracker::set_total(int total) {
    total_files_.store(total, std::memory_order_release);
    is_scanning_.store(0, std::memory_order_release);
}

void ProgressTracker::increment_scanned() {
    scanned_files_.fetch_add(1, std::memory_order_relaxed);
}

void ProgressTracker::increment_processed(const std::string& current_file) {
    processed_files_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(current_file_mu_);
        current_file_ = current_file;
    }
}

void ProgressTracker::increment_integrated() {
    integrated_files_.fetch_add(1, std::memory_order_relaxed);
}

void ProgressTracker::add_error(Error err) {
    std::lock_guard lock(errors_mu_);
    errors_.push_back(std::move(err));
}

int ProgressTracker::integrated_count() const {
    return static_cast<int>(integrated_files_.load(std::memory_order_relaxed));
}

IndexingProgress ProgressTracker::get_progress() const {
    auto total = total_files_.load(std::memory_order_acquire);
    auto processed = processed_files_.load(std::memory_order_acquire);
    auto scanned = scanned_files_.load(std::memory_order_acquire);
    bool scanning = is_scanning_.load(std::memory_order_acquire) == 1;

    std::string file;
    {
        std::lock_guard lock(current_file_mu_);
        file = current_file_;
    }

    auto elapsed = std::chrono::steady_clock::now() - start_time_;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);

    double fps = 0.0;
    int64_t eta_seconds = 0;
    if (processed > 0 && elapsed_ms.count() > 0) {
        fps = static_cast<double>(processed) /
              (static_cast<double>(elapsed_ms.count()) / 1000.0);
        if (fps > 0.0) {
            int64_t remaining = total - processed;
            if (remaining > 0) {
                eta_seconds = static_cast<int64_t>(
                    static_cast<double>(remaining) / fps);
            }
        }
    }

    std::vector<Error> errors_copy;
    {
        std::lock_guard lock(errors_mu_);
        errors_copy = errors_;
    }

    double scan_pct = 0.0;
    double index_pct = 0.0;
    if (scanning) {
        if (scanned > 0) {
            scan_pct = kEstimatedScanningProgress;
        }
    } else {
        scan_pct = 100.0;
        if (total > 0) {
            index_pct = static_cast<double>(processed) /
                        static_cast<double>(total) * 100.0;
        }
    }

    return IndexingProgress{
        .files_processed = static_cast<int>(processed),
        .total_files = static_cast<int>(total),
        .files_scanned = static_cast<int>(scanned),
        .current_file = std::move(file),
        .files_per_second = fps,
        .estimated_time_left = std::chrono::seconds(eta_seconds),
        .errors = std::move(errors_copy),
        .scanning_progress = scan_pct,
        .indexing_progress = index_pct,
        .is_scanning = scanning,
        .elapsed = elapsed_ms,
    };
}

}  // namespace lci
