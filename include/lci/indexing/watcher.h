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
#include <lci/indexing/debounced_rebuilder.h>
#include <lci/indexing/deleted_file_tracker.h>
#include <lci/indexing/pipeline_scanner.h>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

namespace lci {

class MasterIndex;

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
    /// `old_filename` is non-empty only for Rename events: efsw's previous
    /// name for the entry. The watcher then emits a Remove for the old path
    /// before the Rename for the new one.
    void on_efsw_event(const std::string& dir, const std::string& filename,
                       FileEventType type,
                       const std::string& old_filename = std::string());

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

/// Wires the watch path end to end: FileWatcher events on the project root
/// are debounced and applied to a MasterIndex as incremental
/// index_file/update_file/remove_file writes, so a running server serves
/// fresh results without a manual /reindex.
///
/// Event policy (all writes run on the DebouncedRebuilder's timer thread —
/// the efsw callback thread never touches the index, because the
/// single-file write APIs acquire MasterIndex::bulk_mu_ and would block on
/// a bulk reindex window for minutes, overflowing the inotify queue):
///   - Remove: the path is queued in pending_paths_ (batched under
///     kPathBatchId); at rebuild time the file id is recorded in a
///     DeletedFileTracker snapshot and the file removed from the index.
///   - Create/Write/Rename of a NOT-yet-indexed path: queued in
///     pending_paths_, then applied via index_file (a new path has no
///     FileID to debounce on).
///   - Write/Rename of an indexed path: debounced per FileID, then applied
///     via update_file with content read from disk.
///
/// Mutual exclusion with a bulk reindex is structural, not checked here:
/// MasterIndex's single-file writers acquire bulk_mu_ and wait out the
/// bulk clear->publish window (see the INVARIANT in master_index.cpp), so
/// rebuilds are applied, never rescheduled or dropped.
///
/// Exclude/gitignore rules from the Config (.lci.kdl) are enforced by the
/// FileWatcher itself before any event reaches this pipeline.
class WatchPipeline {
  public:
    WatchPipeline(const Config& config, MasterIndex& index);
    ~WatchPipeline();

    WatchPipeline(const WatchPipeline&) = delete;
    WatchPipeline& operator=(const WatchPipeline&) = delete;

    /// Starts watching config.project.root. Returns false when watch_mode
    /// is disabled or the root is invalid (same contract as
    /// FileWatcher::start).
    bool start();

    /// Stops the watcher and drains the debouncer. Idempotent.
    void stop();

    /// Files deleted since start (lock-free read snapshot).
    const DeletedFileTracker& deleted_files() const { return deleted_; }

  private:
    /// Sentinel FileID scheduling a drain of pending_paths_: Remove and
    /// new-path events have no (stable) FileID to debounce on, so they
    /// share one batch slot. Real FileIDs start at 1.
    static constexpr FileID kPathBatchId{0};

    void on_event(const std::string& path, FileEventType type);
    void on_rebuild(const std::vector<FileID>& file_ids);
    /// Reconciles one queued path against disk and index (remove / fresh
    /// add / update). Runs on the debouncer timer thread; may block on
    /// MasterIndex::bulk_mu_.
    void apply_path(const std::string& path);

    const Config& config_;
    MasterIndex& index_;
    FileWatcher watcher_;
    DebouncedRebuilder rebuilder_;
    DeletedFileTracker deleted_;

    // Debounce batch -> path resolution. The DebouncedRebuilder keys on
    // FileID; the path is needed to reload content when the batch fires.
    std::mutex paths_mu_;
    absl::flat_hash_map<FileID, std::string> scheduled_paths_;
    /// Paths queued without a FileID (Remove events, not-yet-indexed
    /// paths), drained when the kPathBatchId batch fires.
    absl::flat_hash_set<std::string> pending_paths_;
};

}  // namespace lci
