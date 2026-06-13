#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/config.h>
#include <lci/core/atomic_shared_ptr.h>
#include <lci/core/file_content_store.h>
#include <lci/core/file_service.h>
#include <lci/core/reference_tracker.h>
#include <lci/core/symbol_store.h>
#include <lci/core/trigram.h>
#include <lci/indexing/index_locks.h>
#include <lci/indexing/pipeline.h>
#include <lci/indexing/pipeline_progress.h>
#include <lci/search/search_options.h>
#include <lci/types.h>

namespace lci {

/// Immutable snapshot of file path <-> FileID mappings.
///
/// Snapshots are swapped atomically so readers never block.
/// Writers create a copy-on-write clone, modify it, and store it atomically.
struct FileSnapshot {
    absl::flat_hash_map<std::string, FileID> file_map;
    absl::flat_hash_map<FileID, std::string> reverse_file_map;

    int file_count() const {
        return static_cast<int>(file_map.size());
    }
};

/// Statistics for the MasterIndex.
struct MasterIndexStats {
    int total_files{};
    int total_symbols{};
    int total_references{};
    int64_t indexing_time_ns{};
    bool is_indexing{};
    int64_t processed_files{};
    int64_t total_files_to_process{};
};

/// Owns all sub-indexes and orchestrates indexing.
///
/// The MasterIndex is the top-level coordinator that:
///   - Owns TrigramIndex, ReferenceTracker, PostingsIndex, SymbolLocationIndex,
///     FileContentStore, and their supporting state.
///   - Manages the file snapshot (path <-> FileID) with atomic swap for lock-free reads.
///   - Coordinates full directory indexing through the Pipeline.
///   - Supports single-file IndexFile / UpdateFile / RemoveFile with fine-grained locking.
///   - Tracks progress atomically (isIndexing, totalFiles, processedFiles).
///
/// Thread safety:
///   - read_snapshot() is lock-free.
///   - index_directory() holds bulk_mu_ (blocks other bulk ops, not reads).
///   - index_file / update_file / remove_file hold snapshot_mu_ and per-index write locks.
///   - clear() holds both mu_ and snapshot_mu_.
class MasterIndex {
  public:
    explicit MasterIndex(const Config& config);
    ~MasterIndex();

    MasterIndex(const MasterIndex&) = delete;
    MasterIndex& operator=(const MasterIndex&) = delete;

    // -- Directory indexing ---------------------------------------------------

    /// Indexes all files in a directory tree using the pipeline.
    /// Blocks until indexing completes. Returns false if already indexing.
    bool index_directory(const std::string& root);

    /// Requests cooperative cancellation of any in-flight `index_directory`
    /// call. Safe to call from any thread. The flag is sticky for the
    /// duration of the active run; the next `index_directory` call clears
    /// it on entry. If no run is active, marks the next run to abort
    /// at its earliest checkpoint.
    void request_stop();

    /// Returns true if cancellation has been requested for the current or
    /// most recent indexing run. Cleared at the start of the next run.
    bool stop_requested() const;

    // -- Single-file operations -----------------------------------------------

    /// Indexes a single file into the index.
    bool index_file(const std::string& path);

    /// Updates a file in the index with new content.
    bool update_file(const std::string& path, std::string_view content);

    /// Removes a file from the index.
    bool remove_file(const std::string& path);

    // -- Lifecycle -------------------------------------------------------------

    /// Clears all indexed data. Fails if indexing is in progress.
    bool clear();

    // -- Lock-free reads ------------------------------------------------------

    /// Returns the current file snapshot for lock-free reads.
    std::shared_ptr<const FileSnapshot> read_snapshot() const;

    /// Returns the FileID for a path, or 0 if not found.
    FileID path_to_id(const std::string& path) const;

    /// Returns the path for a FileID, or empty string if not found.
    std::string id_to_path(FileID file_id) const;

    // -- Statistics ------------------------------------------------------------

    MasterIndexStats get_stats() const;
    int file_count() const;
    bool is_indexing() const;

    /// Phase of the indexing pipeline as observed by external monitors
    /// such as /status. Maps the underlying ProgressTracker state into
    /// the four-state machine that callers expose to users.
    enum class IndexingPhase {
        Idle,      // no run active
        Scanning,  // file discovery (Scanner stage)
        Indexing,  // processing files (Processor stage)
        Merging,   // post-scan, all files processed but run still
                   // wrapping up (Integrator drain / postings flush)
    };

    /// Live snapshot of indexing progress designed for /status polling.
    ///
    /// All fields are 0 / Idle when no run is active. When a run is in
    /// flight the snapshot is read directly from the active Pipeline's
    /// ProgressTracker, which uses atomics on the hot path so reads
    /// don't block writers.
    struct IndexingProgressSnapshot {
        IndexingPhase phase{IndexingPhase::Idle};
        int files_scanned{0};
        int files_total{0};
        int percent_complete{0};  // clamped to [0, 100]
        int64_t elapsed_ms{0};
    };

    /// Returns a snapshot of the current indexing run, or an idle
    /// snapshot when no run is in flight. Thread-safe — readers may
    /// poll while the pipeline is active without racing the writer.
    IndexingProgressSnapshot get_progress() const;

    // -- Sub-index access (non-owning) ----------------------------------------

    TrigramIndex& trigram_index();
    const TrigramIndex& trigram_index() const;

    ReferenceTracker& ref_tracker();
    const ReferenceTracker& ref_tracker() const;

    PostingsIndex& postings_index();
    const PostingsIndex& postings_index() const;

    SymbolLocationIndex& symbol_location_index();
    const SymbolLocationIndex& symbol_location_index() const;

    FileContentStore& file_content_store();
    const FileContentStore& file_content_store() const;
    std::shared_ptr<FileContentStore> file_content_store_ptr();

    const Config& config() const;

    // -- Search dispatch ------------------------------------------------------

    /// Searches for a pattern across all indexed files.
    /// Returns results with optional context lines.
    std::vector<SearchResult> search(const std::string& pattern,
                                     int max_context_lines) const;

    /// Searches with full options control.
    std::vector<SearchResult> search_with_options(
        const std::string& pattern,
        const SearchOptions& options) const;

    /// Returns candidate file IDs matching a pattern's trigrams.
    std::vector<FileID> find_candidate_files(
        const std::string& pattern, bool case_insensitive) const;

    /// Searches for symbol definitions (declarations only).
    std::vector<SearchResult> search_definitions(
        const std::string& pattern) const;

    /// Searches for symbol references (usages only).
    std::vector<SearchResult> search_references(
        const std::string& symbol) const;

    /// Returns the file path for a given FileID (alias for id_to_path).
    std::string get_file_path(FileID file_id) const;

    /// Returns all non-deleted file IDs.
    std::vector<FileID> get_all_file_ids() const;

  private:
    Config config_;

    // Sub-indexes (owned)
    TrigramIndex trigram_index_;
    SymbolLocationIndex symbol_location_index_;
    ReferenceTracker ref_tracker_;
    PostingsIndex postings_index_;
    std::shared_ptr<FileContentStore> file_content_store_;
    std::shared_ptr<FileService> file_service_;

    // Lock management (mutable: locking is internal state for const search ops).
    // Guards Symbol, Reference, Location, and Content only. Trigram and Postings
    // are lock-free RCU (own write_mu_ + atomic snapshot publish), so they take
    // no guard here on either the read or single-file write path.
    mutable IndexLockManager lock_manager_;

    // File snapshot (atomic swap for lock-free reads)
    AtomicSharedPtr<const FileSnapshot> snapshot_;

    // Fine-grained locks
    std::mutex snapshot_mu_;  // lightweight lock for snapshot updates
    std::mutex bulk_mu_;      // heavy lock for bulk operations

    // Atomic state
    std::atomic<int32_t> is_indexing_{0};
    std::atomic<int64_t> total_files_{0};
    std::atomic<int64_t> processed_files_{0};
    std::atomic<int64_t> indexing_time_ns_{0};
    mutable std::atomic<int64_t> search_count_{0};

    // Cancellation. `stop_requested_` is the persistent user-visible
    // signal forwarded into the active `Pipeline`. `active_pipeline_` is
    // a non-owning pointer set while `index_directory` is running so
    // `request_stop()` can forward immediately; protected by
    // `stop_mu_`.
    std::atomic<bool> stop_requested_{false};
    Pipeline* active_pipeline_{nullptr};
    mutable std::mutex stop_mu_;

    // Helpers
    void set_bulk_indexing(bool enabled);
    void update_snapshot_for_file(const std::string& path,
                                  FileID new_id, FileID old_id,
                                  bool existed);
    void remove_file_from_indexes(FileID file_id, const std::string& path);
    std::shared_ptr<const FileSnapshot> load_snapshot() const;

    // Search helpers (in master_index_search.cpp)
    std::string validate_search_input(const std::string& pattern,
                                       SearchOptions& options) const;
    std::string validate_search_components() const;
    std::vector<SearchResult> execute_search(
        const std::string& pattern,
        const std::vector<FileID>& candidates,
        const SearchOptions& options) const;
    SearchContext extract_context(FileID file_id, int match_line,
                                  int max_context_lines) const;
};

}  // namespace lci
