#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/core/atomic_shared_ptr.h>
#include <lci/string_ref.h>
#include <lci/types.h>

namespace lci {

/// Holds the actual content and pre-computed line information for a single file.
/// Immutable once constructed; updates create new instances.
struct FileContent {
    FileID file_id{};
    std::vector<uint8_t> content;
    std::vector<uint32_t> line_offsets;
    uint64_t fast_hash{};
    std::array<uint8_t, 32> content_hash{};
    std::atomic<int32_t> ref_count{1};

    FileContent() = default;
    FileContent(const FileContent&) = delete;
    FileContent& operator=(const FileContent&) = delete;

    /// Returns the content as a string_view.
    std::string_view view() const {
        return {reinterpret_cast<const char*>(content.data()), content.size()};
    }
};

/// Immutable snapshot of the file content store.
/// Holds the mapping from FileID to FileContent and path-to-ID lookup.
/// Snapshots are swapped atomically for lock-free reads.
///
/// Lookup performance:
///   - find_by_id / find_by_path / path_to_id are O(1) average via the
///     id_index_ / path_index_ hash maps that store entry positions.
///   - The index maps must be kept in sync with `entries` whenever entries
///     are added, replaced, or erased. Use rebuild_indices() after any
///     mutation that may invalidate positions (e.g. erase / LRU eviction)
///     and update_index_for_*() helpers for in-place edits.
struct FileContentSnapshot {
    struct Entry {
        FileID file_id{};
        std::string path;
        std::shared_ptr<FileContent> content;
    };

    std::vector<Entry> entries;
    std::vector<FileID> access_order;

    /// Position-in-entries indexes for O(1) lookup. Both maps must be
    /// kept consistent with `entries` after any mutation.
    absl::flat_hash_map<FileID, size_t> id_index;
    absl::flat_hash_map<std::string, size_t> path_index;

    /// Finds a FileContent by file ID. Returns nullptr if not found.
    const std::shared_ptr<FileContent>& find_by_id(FileID id) const;

    /// Finds a FileContent by path. Returns nullptr if not found.
    const std::shared_ptr<FileContent>& find_by_path(const std::string& path) const;

    /// Returns the FileID for a path, or 0 if not found.
    FileID path_to_id(const std::string& path) const;

    /// Returns the number of files in the snapshot.
    size_t file_count() const { return entries.size(); }

    /// Rebuilds id_index / path_index from `entries`. Call after any mutation
    /// that may shift positions (erase, LRU eviction, bulk rebuilds).
    void rebuild_indices();
};

/// Classification of update operations.
enum class UpdateType : uint8_t {
    Load = 0,
    Batch,
    Invalidate,
    InvalidateByID,
    Clear,
};

/// Result of an update operation.
struct UpdateResult {
    FileID file_id{};
    std::vector<FileID> file_ids;
    bool success{};
    std::string error;
};

/// Request for a content update.
struct ContentUpdate {
    UpdateType type{};
    std::string path;
    std::vector<uint8_t> content;
    FileID file_id{};
    std::vector<std::pair<std::string, std::vector<uint8_t>>> batch_data;
};

/// Manages all file content with lock-free concurrent read access.
///
/// Architecture:
///   - Concurrent reads via atomic shared_ptr snapshot (no locks)
///   - Writes serialized through a single update method
///   - Immutable snapshots swapped atomically
///
/// Performance:
///   - Lock-free reads with atomic load
///   - Memory-efficient with LRU eviction
class FileContentStore {
  public:
    /// Default memory limit: 500 MB.
    static constexpr int64_t kDefaultMaxMemory = 500 * 1024 * 1024;

    explicit FileContentStore(int64_t max_memory_bytes = kDefaultMaxMemory);
    ~FileContentStore() = default;

    FileContentStore(const FileContentStore&) = delete;
    FileContentStore& operator=(const FileContentStore&) = delete;

    // -- Lock-free read API ---------------------------------------------------

    /// Returns the FileContent for a file ID, or nullptr.
    std::shared_ptr<FileContent> get_file(FileID id) const;

    /// Returns the raw content bytes for a file.
    std::string_view get_content(FileID id) const;

    /// Returns the precomputed line offsets for a file.
    const std::vector<uint32_t>* get_line_offsets(FileID id) const;

    /// Resolves a StringRef to a string_view from the backing content.
    std::string_view get_string(StringRef ref) const;

    /// Returns a StringRef for a specific line (0-based).
    StringRef get_line(FileID file_id, int line_num) const;

    /// Returns the number of lines in a file.
    int get_line_count(FileID file_id) const;

    /// Returns the pre-computed SHA256 hash for a file.
    std::array<uint8_t, 32> get_content_hash(FileID file_id) const;

    /// Returns the pre-computed xxHash for a file.
    uint64_t get_fast_hash(FileID file_id) const;

    /// Returns the number of files in the store.
    int get_file_count() const;

    /// Returns current memory usage in bytes.
    int64_t get_memory_usage() const;

    /// Returns the FileID for a path, or 0 if not found.
    FileID path_to_id(const std::string& path) const;

    // -- Write API (single-writer, serialized) --------------------------------

    /// Loads a file into the store and returns its ID.
    FileID load_file(const std::string& path, std::string_view content);

    /// Loads multiple files in a single batch.
    std::vector<FileID> batch_load_files(
        const std::vector<std::pair<std::string, std::string_view>>& files);

    /// Removes a file from the store by path.
    void invalidate_file(const std::string& path);

    /// Removes a file from the store by ID.
    void invalidate_file_by_id(FileID file_id);

    /// Removes all files from the store.
    void clear();

  private:
    /// Current snapshot, atomically swapped on writes.
    AtomicSharedPtr<FileContentSnapshot> snapshot_;

    /// Serializes write-side snapshot mutations. Reads are lock-free
    /// against the atomic shared_ptr; the mutex only orders the
    /// load-mutate-swap dance in load_file / batch_load_files /
    /// invalidate_* / clear so concurrent writers cannot lose entries.
    /// Without it, two threads racing through `load_file` both clone the
    /// same prior snapshot, append independently, and the second store
    /// drops whatever the first appended.
    mutable std::mutex write_mu_;

    /// Memory tracking.
    std::atomic<int64_t> current_memory_{0};
    int64_t max_memory_bytes_;

    /// FileID generation.
    std::atomic<uint32_t> next_id_{0};

    /// Loads the current snapshot for reading.
    std::shared_ptr<FileContentSnapshot> load_snapshot() const;

    /// Creates a new FileContent from raw data.
    std::shared_ptr<FileContent> make_file_content(FileID id, std::string_view data) const;

    /// Computes the estimated memory footprint of a FileContent.
    static int64_t estimate_memory(const FileContent& fc);

    /// Performs LRU eviction if memory exceeds the limit.
    void enforce_memory_limit(std::shared_ptr<FileContentSnapshot>& snap);
};

/// Computes byte offsets for each line in the content.
/// Matches Go's computeLineOffsets exactly (CRLF-aware).
std::vector<uint32_t> compute_line_offsets(std::string_view content);

}  // namespace lci
