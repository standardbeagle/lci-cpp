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
    /// Per-file attribute tag (PathAttr), computed ONCE on the indexing
    /// write path by the config-aware PathClassifier. Files absent from the
    /// map are Production. Lock-free O(1) read; readers never re-run globs.
    absl::flat_hash_map<FileID, PathAttrId> file_attrs;

    /// File ids whose attribute activates Capability::Search, sorted.
    /// Precomputed at publish because it is read on EVERY query and changes
    /// only when the index does. Deriving it per search cost ~10ms on the
    /// fastapi corpus: each file needed a file_attrs probe, and both
    /// search() and find_candidate_files() paid it independently.
    std::vector<FileID> searchable_ids;

    PathAttrId attr_of(FileID id) const {
        auto it = file_attrs.find(id);
        return it != file_attrs.end() ? it->second : kFallbackAttr;
    }

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
    /// Loads the snapshot and copies the path out; for hot paths that resolve
    /// many ids per query, prefer the snapshot-scoped overload below.
    std::string id_to_path(FileID file_id) const;

    /// Snapshot-scoped path lookup: returns a string_view into `snap`'s
    /// reverse_file_map with no atomic load and no allocation. The caller holds
    /// `snap` (read_snapshot()) for the view's lifetime — load once per query
    /// and reuse across all id resolutions instead of paying a snapshot load +
    /// string copy per result.
    std::string_view id_to_path(const FileSnapshot& snap, FileID file_id) const;

    // -- Statistics ------------------------------------------------------------

    MasterIndexStats get_stats() const;
    int file_count() const;

    /// Estimated bytes held by the index structures: interned postings,
    /// stored references + name pool, enhanced symbols, and the content
    /// store (mmap-retained content counts at its mapped size). A content
    /// census, not container overhead — same convention as
    /// `lci debug memprofile`. Replaces the /stats field that was
    /// hardcoded to 0.
    size_t index_size_bytes() const;
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

    /// Returns the files a literal-pattern scan must visit: every indexed
    /// file except those an index with coverage certifies pattern-free
    /// (trigram narrowing over its covered files; postings token-run
    /// narrowing — see TrigramIndex::narrow / PostingsIndex::narrow).
    /// When `informative` is non-null it reports whether ANY certification
    /// applied; false means the (possibly empty) return simply mirrors the
    /// full candidate set and the caller cannot treat emptiness as "no
    /// possible match".
    std::vector<FileID> find_candidate_files(
        const std::string& pattern, bool case_insensitive,
        bool* informative = nullptr) const;

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

    /// Lock-free per-file attribute lookup (production/test/example/
    /// vendored/generated/docs). Computed once at index time; see
    /// FileSnapshot::file_attrs. For many lookups, prefer
    /// read_snapshot()->attr_of(id).
    PathAttrId get_file_attr(FileID file_id) const;

    /// The attribute set in force for this index (shipped ruleset + config).
    const PathAttrRegistry& attr_registry() const { return attr_registry_; }

    /// Returns the subset of `scopes` (root-relative file or directory-prefix
    /// tokens, the `lci grep/search <path>...` positional) that match NO
    /// indexed file. An empty result means every scope matches at least one
    /// indexed file. Used to fail fast when a user-supplied path exists on
    /// disk but was never indexed (gitignored, wrong extension, outside the
    /// configured root) — a bare std::filesystem::exists check on the CLI
    /// side cannot detect that, so the path-scope filter would otherwise
    /// empty the candidate set and exit 0 with zero results (silent empty).
    std::vector<std::string> scopes_without_indexed_match(
        const std::vector<std::string>& scopes) const;

  private:
    Config config_;

    /// File attribute classifier (builtins + `.lci.kdl` attributes rules).
    /// Runs only on the indexing write path.
    PathAttrRegistry attr_registry_;
    PathClassifier path_classifier_;

    /// Classifies `path` (absolute or root-relative) against the project
    /// root, consulting indexed content for the minified/generated-header
    /// heuristics when `file_id` is valid.
    PathAttrId classify_file_attr(const std::string& path, FileID file_id) const;

    /// Repo-relative view of `path` for the classifier.
    std::string_view relative_to_project_root(std::string_view path) const;

    // Sub-indexes (owned)
    TrigramIndex trigram_index_;
    SymbolLocationIndex symbol_location_index_;
    ReferenceTracker ref_tracker_;
    PostingsIndex postings_index_;
    std::shared_ptr<FileContentStore> file_content_store_;
    std::shared_ptr<FileService> file_service_;

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
    /// File ids whose attribute activates the Search capability. O(1) —
    /// returns the snapshot's precomputed set.
    std::vector<FileID> searchable_file_ids() const;

    /// Computes the snapshot's derived views and publishes it. Every
    /// snapshot store goes through here so no publish path can forget one.
    void publish_snapshot(std::shared_ptr<FileSnapshot> snap);

    std::vector<SearchResult> execute_search(
        const std::string& pattern,
        const std::vector<FileID>& candidates,
        const SearchOptions& options) const;
    SearchContext extract_context(FileID file_id, int match_line,
                                  int max_context_lines) const;
};

}  // namespace lci
