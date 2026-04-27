#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include <lci/types.h>

namespace lci {

/// Immutable snapshot of deleted file IDs for lock-free reads.
/// Uses copy-on-write: writers create a new snapshot and swap atomically.
struct DeletedFileSet {
    absl::flat_hash_set<FileID> files;

    bool contains(FileID file_id) const {
        return files.contains(file_id);
    }

    int size() const {
        return static_cast<int>(files.size());
    }
};

/// Tracks files deleted between full index rebuilds using lock-free reads.
///
/// Writers use compare-and-swap on an atomic shared_ptr to publish new
/// immutable snapshots.  Readers load the current snapshot without locking.
/// This mirrors the Go implementation's atomic.Pointer[DeletedFileSet] pattern.
class DeletedFileTracker {
  public:
    DeletedFileTracker();

    /// Marks a single file as deleted (CAS loop, lock-free).
    void mark_deleted(FileID file_id);

    /// Marks multiple files as deleted in one CAS (lock-free).
    void mark_deleted_batch(const std::vector<FileID>& file_ids);

    /// Returns true if the file has been marked deleted (lock-free read).
    bool is_deleted(FileID file_id) const;

    /// Returns candidates with deleted files removed.
    std::vector<FileID> filter_candidates(
        const std::vector<FileID>& candidates) const;

    /// Clears all tracked deletions (called after full reindex).
    void clear();

    /// Returns the number of files currently marked as deleted.
    int deleted_count() const;

    /// Returns a copy of all deleted file IDs (for diagnostics).
    std::vector<FileID> deleted_file_ids() const;

  private:
    std::atomic<std::shared_ptr<const DeletedFileSet>> snapshot_;
    std::shared_ptr<const DeletedFileSet> load() const;
};

}  // namespace lci
