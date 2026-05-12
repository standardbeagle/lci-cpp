#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include <lci/config.h>
#include <lci/config/gitignore.h>
#include <lci/indexing/binary_detector.h>
#include <lci/indexing/pipeline_types.h>

namespace lci {

/// Traverses directories to discover source files for indexing.
///
/// Respects .gitignore patterns, detects binary files, handles symlink
/// cycles, and supports glob-based inclusion/exclusion filters.
class FileScanner {
  public:
    explicit FileScanner(const Config& config);

    /// Scans the project root and returns all indexable file tasks.
    std::vector<FileTask> scan();

    /// Simple glob match supporting `*`, `**`, and `?` patterns.
    ///
    /// **Path contract:** `path` must be **relative-to-project-root**.
    /// Pattern semantics:
    ///   - `?`  matches any single non-`/` character
    ///   - `*`  matches zero or more non-`/` characters (component-local)
    ///   - `**` matches zero or more characters across `/` boundaries
    ///   - all other characters are literals
    ///
    /// Passing an absolute path produces over-matching for patterns
    /// anchored with `**/` because the leading prefix (e.g. `/tmp/...`)
    /// is treated as a prefix component. All callers in this codebase
    /// pass relative paths; new callers must do the same.
    static bool match_glob(std::string_view pattern, std::string_view path);

  private:
    const Config& config_;
    BinaryDetector binary_detector_;
    GitignoreParser gitignore_parser_;
    std::vector<std::string> exclusions_;
    std::vector<std::string> inclusions_;

    /// Recursively walks a directory, tracking visited inodes for cycle detection.
    void walk_directory(const std::filesystem::path& dir,
                        absl::flat_hash_set<uint64_t>& visited_inodes,
                        std::vector<FileTask>& out);

    /// Returns true if the path matches any exclusion pattern.
    bool should_exclude(std::string_view rel_path) const;

    /// Returns true if the path matches an inclusion pattern (or no inclusions set).
    bool should_include(std::string_view rel_path) const;

    /// Returns true if a regular file should be processed.
    bool should_process_file(const std::filesystem::path& path,
                             std::string_view rel_path,
                             int64_t file_size) const;

    /// Assigns a processing priority based on file extension.
    static int get_file_priority(std::string_view path);

    /// Detects the language from a file extension.
    static std::string detect_language(std::string_view path);

};

}  // namespace lci
