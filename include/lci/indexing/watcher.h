#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <lci/config.h>
#include <lci/config/gitignore.h>
#include <lci/indexing/pipeline_scanner.h>

namespace lci {

/// Type of file system event detected by the watcher.
enum class FileEventType : uint8_t {
    Create = 0,
    Write,
    Remove,
    Rename,
};

/// Statistics for file watching operations.
struct WatchStats {
    int64_t events_processed{};
    int64_t error_count{};
    std::chrono::steady_clock::time_point last_event_time{};
    bool is_active{};
};

/// Cross-platform file watcher backed by efsw (Linux inotify, macOS FSEvents,
/// Windows ReadDirectoryChangesW).
///
/// Replaces the prior hand-rolled per-platform implementation that leaked
/// inotify watch descriptors on abnormal shutdown.  efsw RAII-owns its
/// platform handles, so destruction releases all kernel resources.
///
/// Thread safety: Start/Stop are not concurrent-safe with each other.
/// Callbacks may be invoked from efsw's internal worker thread.
class FileWatcher {
  public:
    using EventCallback = std::function<void(const std::string& path,
                                             FileEventType event)>;

    explicit FileWatcher(const Config& config);
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    /// Sets the callback invoked for each filtered file event.
    void set_callback(EventCallback cb);

    /// Starts watching the given root directory recursively.
    /// Returns false if watch mode is disabled or the root is invalid.
    bool start(const std::string& root);

    /// Stops the watcher and releases all kernel watch handles.
    void stop();

    /// Returns current watch statistics.
    WatchStats get_stats() const;

    // -- Internal: invoked by the efsw listener adapter.  Public so the
    // -- adapter (defined in the .cpp) can dispatch without friend coupling.
    void on_efsw_event(const std::string& dir, const std::string& filename,
                       FileEventType type);

  private:
    bool should_ignore_dir(const std::string& path) const;
    bool should_process_path(const std::string& path) const;
    void dispatch_event(const std::string& path, FileEventType type);

    const Config& config_;
    GitignoreParser gitignore_;
    EventCallback callback_;

    // Forward-declared platform/efsw state — defined in the .cpp to avoid
    // leaking efsw headers into the include tree.
    struct WatcherState;
    std::unique_ptr<WatcherState> state_;
    std::atomic<bool> running_{false};

    // Stats (mutable for const get_stats)
    mutable std::mutex stats_mu_;
    int64_t events_processed_{};
    int64_t error_count_{};
    std::chrono::steady_clock::time_point last_event_time_{};
};

}  // namespace lci
