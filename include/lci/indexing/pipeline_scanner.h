#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include <lci/config.h>
#include <lci/path_classifier.h>
#include <lci/config/gitignore.h>
#include <lci/indexing/binary_detector.h>
#include <lci/indexing/pipeline_types.h>

namespace lci {

/// Traverses directories to discover source files for indexing.
///
/// Respects .gitignore patterns, detects binary files, handles symlink
/// cycles, and supports glob-based inclusion/exclusion filters.
/// Outcome of a scan, including how the corpus budget was applied.
struct ScanResult {
    std::vector<FileTask> tasks;
    /// Files dropped by the "reduced" overflow policy (0 when within budget).
    int skipped_files{};
    int64_t skipped_bytes{};
    /// Non-empty when the "reject" overflow policy tripped; tasks is empty.
    std::string error;
};

class FileScanner {
  public:
    explicit FileScanner(const Config& config);

    /// Scans the project root and returns indexable file tasks, budgeted by
    /// index.max_total_size_mb / index.max_file_count in priority order:
    /// the highest-priority files fill the budget first. Past the budget,
    /// index.overflow_policy decides: "reduced" truncates and reports the
    /// skip counts; "reject" returns an error and no tasks.
    ///
    /// apply_budget=false skips the budget entirely — for callers that
    /// stream from disk per file (grep full-scan) where a partial file list
    /// would silently hide matches and memory is not at stake.
    ScanResult scan(bool apply_budget = true);

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
    /// The attribute set in force (shipped ruleset + this project's
    /// `attributes` block). The Index capability decides whether a scanned
    /// file is offered to the pipeline at all.
    std::string attr_error_;
    PathAttrRegistry attr_registry_;
    PathClassifier attr_classifier_;
    const Config& config_;
    BinaryDetector binary_detector_;
    GitignoreParser gitignore_parser_;
    /// A glob plus its longest literal (wildcard-free) run. Any path the
    /// glob matches must contain the literal verbatim, so a cheap
    /// `find` rejects most candidates before the backtracking matcher runs
    /// — match_glob_at alone was 25% of scan CPU on a 55k-file corpus.
    struct CompiledGlob {
        std::string pattern;
        std::string literal;
    };
    std::vector<CompiledGlob> exclusions_;
    std::vector<CompiledGlob> inclusions_;

    static CompiledGlob compile_glob(std::string pattern);
    static bool matches_compiled(const CompiledGlob& glob,
                                 std::string_view rel_path);

    /// Recursively walks a directory, tracking visited inodes for cycle
    /// detection. `rel_prefix` is the directory's repo-relative path (""
    /// at the root): child paths are derived by appending the entry name,
    /// NOT via std::filesystem::relative — fs::relative canonicalizes both
    /// sides with a readlink per path component, which was 39% of scan
    /// wall on a 55k-file corpus.
    void walk_directory(const std::filesystem::path& dir,
                        const std::string& rel_prefix,
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
