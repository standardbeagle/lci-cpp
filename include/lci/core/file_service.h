#pragma once

#include <lci/core/file_content_store.h>
#include <lci/error.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lci {

/// Facade over FileContentStore providing file loading with size enforcement
/// and line content access.
///
/// This is the primary entry point for loading files and accessing their
/// content and line information. It delegates storage to FileContentStore.
class FileService {
  public:
    /// Default maximum file size: 10 MB.
    static constexpr int64_t kDefaultMaxFileSize = 10 * 1024 * 1024;

    explicit FileService(
        std::shared_ptr<FileContentStore> store = nullptr,
        int64_t max_file_size_bytes = kDefaultMaxFileSize);

    /// Loads file content from a buffer, enforcing the max file size.
    /// Returns the FileID on success, or an error if too large.
    Result<FileID> add_file(const std::string& path, std::string_view content);

    /// Loads a file from disk, enforcing max file size. The read uses a
    /// TRANSIENT memory mapping and the store copies the bytes at index
    /// time; no mapping outlives this call. Returns the FileID or an error.
    Result<FileID> load_file_from_disk(const std::string& path);

    /// Batches multiple from-disk loads into a single snapshot rewrite.
    /// Each per-file add_file rewrites the FileContentSnapshot end-to-end
    /// (RCU copy-on-write); calling that N times is O(N²) in snapshot
    /// size — the pipeline producer used to pay this for every scanned
    /// file. batch_load_from_disk mmap-opens each path, enforces the size
    /// limit, then submits the survivors as one batch. Each mapping is
    /// transient: the store copies the bytes, and every mapping is unmapped
    /// before this returns.
    ///
    /// Returns FileIDs in input order; 0 for files that failed to open or
    /// exceeded the size limit. When `failures` is non-null, each such file
    /// pushes an Error onto it — a load failure is never silently dropped.
    std::vector<FileID> batch_load_from_disk(
        const std::vector<std::string>& paths,
        std::vector<Error>* failures = nullptr);

    /// Returns the raw content for a file, or empty if not found.
    std::string_view get_content(FileID id) const;

    /// Returns a specific line (0-based) as a string_view with CRLF stripped.
    /// Returns empty view if file or line not found.
    std::string_view get_line_content(FileID id, int line) const;

    /// Returns the number of lines in a file, or 0 if not found.
    int get_line_count(FileID id) const;

    /// Returns the underlying content store.
    FileContentStore& store();
    const FileContentStore& store() const;

    /// Returns the configured max file size in bytes.
    int64_t max_file_size_bytes() const;

  private:
    std::shared_ptr<FileContentStore> store_;
    int64_t max_file_size_bytes_;
};

}  // namespace lci
