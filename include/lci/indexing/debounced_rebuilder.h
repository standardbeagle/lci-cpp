#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include <lci/types.h>

namespace lci {

/// Debounces file change events and triggers a single rebuild callback.
///
/// When ScheduleRebuild is called, the rebuilder waits for the debounce
/// period with no new events before firing the callback.  Rapid saves
/// from editors collapse into a single rebuild.
class DebouncedRebuilder {
  public:
    using RebuildCallback = std::function<void(const std::vector<FileID>&)>;

    /// Creates a rebuilder with the given debounce delay (default 50ms).
    explicit DebouncedRebuilder(
        std::chrono::milliseconds debounce = std::chrono::milliseconds{50});

    ~DebouncedRebuilder();

    DebouncedRebuilder(const DebouncedRebuilder&) = delete;
    DebouncedRebuilder& operator=(const DebouncedRebuilder&) = delete;

    /// Sets the callback invoked when the debounce period expires.
    void set_callback(RebuildCallback cb);

    /// Schedules a rebuild for the given file after the debounce period.
    void schedule_rebuild(FileID file_id);

    /// Immediately triggers a rebuild for all pending files.
    void force_rebuild();

    /// Returns the number of files pending rebuild.
    int pending_count() const;

    /// Updates the debounce delay.
    void set_debounce(std::chrono::milliseconds ms);

    /// Shuts down the rebuilder, cancelling any pending timer.
    void shutdown();

  private:
    void timer_thread_func();
    void flush_pending();

    RebuildCallback callback_;
    std::chrono::milliseconds debounce_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    absl::flat_hash_set<FileID> pending_;
    std::chrono::steady_clock::time_point deadline_;
    bool has_pending_{false};

    std::thread timer_thread_;
    bool running_{true};
};

}  // namespace lci
