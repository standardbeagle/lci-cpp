#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <absl/container/flat_hash_set.h>

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

/// Platform-native file watcher that monitors a directory tree for changes.
///
/// Uses inotify on Linux, FSEvents on macOS, and ReadDirectoryChangesW on
/// Windows.  Events are debounced internally before dispatching callbacks.
///
/// Thread safety: Start/Stop are not concurrent-safe with each other.
/// Callbacks may be invoked from internal threads.
class FileWatcher {
  public:
    using EventCallback = std::function<void(const std::string& path,
                                             FileEventType event)>;

    explicit FileWatcher(const Config& config);
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    /// Sets the callback invoked for each debounced file event.
    void set_callback(EventCallback cb);

    /// Starts watching the given root directory recursively.
    /// Returns false if watch mode is disabled or the root is invalid.
    bool start(const std::string& root);

    /// Stops the watcher and waits for all threads to finish.
    void stop();

    /// Returns current watch statistics.
    WatchStats get_stats() const;

#ifdef __APPLE__
    // Called from the FSEvents C trampoline; must be accessible externally.
    void handle_fsevents(size_t num_events, void* event_paths,
                         const unsigned int event_flags[]);
#endif

  private:
    // -- Platform-specific implementation detail --------------------------
    struct PlatformState;

    void event_loop();
    bool add_watches(const std::string& root);
    bool should_ignore_dir(const std::string& path) const;
    bool should_process_path(const std::string& path) const;
    void dispatch_event(const std::string& path, FileEventType type);

    const Config& config_;
    GitignoreParser gitignore_;
    EventCallback callback_;

    std::unique_ptr<PlatformState> platform_;
    std::thread event_thread_;
    std::atomic<bool> running_{false};

    // Stats (mutable for const get_stats)
    mutable std::mutex stats_mu_;
    int64_t events_processed_{};
    int64_t error_count_{};
    std::chrono::steady_clock::time_point last_event_time_{};
};

}  // namespace lci
