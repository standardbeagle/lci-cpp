#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <lci/git/types.h>

namespace lci {
namespace git {

/// Wraps git CLI commands to extract file states at different refs.
/// Ported from Go: internal/git/provider.go
class Provider {
  public:
    /// Creates a new git provider for the specified repository.
    /// Returns false if the path is not a git repository.
    static bool create(std::string_view repo_root, Provider& out);

    /// Checks if the directory is a git repository.
    bool is_git_repo() const;

    /// Returns the repository root path.
    const std::string& repo_root() const { return repo_root_; }

    /// Returns the list of changed files based on analysis scope.
    bool get_changed_files(const AnalysisParams& params,
                           std::vector<ChangedFile>& out) const;

    /// Returns statistics about the diff.
    bool get_diff_stats(const AnalysisParams& params, DiffStats& out) const;

    /// Returns the content of a file at a specific ref.
    /// If ref is empty or "WORKING", returns the working tree version.
    /// If ref is "STAGED" or "INDEX", returns the staged content.
    bool get_file_content(std::string_view ref, std::string_view path,
                          std::string& out) const;

    /// Determines the appropriate base reference for a scope.
    bool get_base_ref(const AnalysisParams& params, std::string& out) const;

    /// Determines the appropriate target reference for a scope.
    std::string get_target_ref(const AnalysisParams& params) const;

    /// Returns all tracked files in the repository.
    bool list_all_files(std::vector<std::string>& out) const;

    /// Returns the current branch name.
    bool get_current_branch(std::string& out) const;

    /// Returns the full commit hash for a reference.
    bool get_commit_hash(std::string_view ref, std::string& out) const;

    /// Runs `git <args...>` in the repo root and captures stdout. Each arg is
    /// shell-quoted, so format strings / refs containing shell metacharacters
    /// (`|`, spaces, `$`, …) are passed literally. Returns true on success.
    bool run_git(const std::vector<std::string>& args, std::string& out) const;

  private:
    std::string repo_root_;

    bool get_staged_files(std::vector<ChangedFile>& out) const;
    bool get_wip_files(std::vector<ChangedFile>& out) const;
    bool get_commit_files(std::string_view commit_ref,
                          std::vector<ChangedFile>& out) const;
    bool get_range_files(std::string_view base_ref, std::string_view target_ref,
                         std::vector<ChangedFile>& out) const;

    bool parse_name_status(std::string_view output,
                           std::vector<ChangedFile>& out) const;
    FileChangeStatus parse_status(std::string_view status) const;
    bool parse_numstat(std::string_view output, DiffStats& out) const;
    bool get_parent_commit(std::string_view commit, std::string& out) const;
    bool get_staged_content(std::string_view path, std::string& out) const;
};

}  // namespace git
}  // namespace lci
