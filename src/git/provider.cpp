#include <lci/git/provider.h>

#include <lci/core/subprocess.h>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace lci {
namespace git {

namespace {

/// Splits a string by newlines, skipping empty lines.
void split_lines(std::string_view input, std::vector<std::string_view>& lines) {
    size_t start = 0;
    while (start < input.size()) {
        size_t end = input.find('\n', start);
        if (end == std::string_view::npos) end = input.size();
        auto line = input.substr(start, end - start);
        if (!line.empty()) lines.push_back(line);
        start = end + 1;
    }
}

/// Splits a line by whitespace into fields.
void split_fields(std::string_view line, std::vector<std::string_view>& fields) {
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        fields.push_back(line.substr(start, i - start));
    }
}

}  // namespace

bool Provider::run_git(const std::vector<std::string>& args,
                       std::string& out) const {
    // No shell: args go to git verbatim (subprocess::run_capture), so
    // `--format=%H|%an|%at` needs no quoting and cannot be injected.
    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.emplace_back("git");
    argv.insert(argv.end(), args.begin(), args.end());
    return subprocess::run_capture(argv, repo_root_, out);
}

bool Provider::create(std::string_view repo_root, Provider& out) {
    std::error_code ec;
    auto abs_root = std::filesystem::absolute(std::string(repo_root), ec);
    if (ec) return false;

    Provider tmp;
    tmp.repo_root_ = abs_root.string();

    std::string git_root;
    if (!tmp.run_git({"rev-parse", "--show-toplevel"}, git_root)) return false;

    // Trim trailing whitespace
    while (!git_root.empty() && (git_root.back() == '\n' || git_root.back() == '\r')) {
        git_root.pop_back();
    }

    tmp.repo_root_ = std::move(git_root);
    out = std::move(tmp);
    return true;
}

bool Provider::is_git_repo() const {
    auto git_dir = std::filesystem::path(repo_root_) / ".git";
    return std::filesystem::is_directory(git_dir);
}

bool Provider::get_changed_files(const AnalysisParams& params,
                                 std::vector<ChangedFile>& out) const {
    switch (params.scope) {
        case AnalysisScope::Staged: return get_staged_files(out);
        case AnalysisScope::WIP: return get_wip_files(out);
        case AnalysisScope::Commit: return get_commit_files(params.base_ref, out);
        case AnalysisScope::Range:
            return get_range_files(params.base_ref, params.target_ref, out);
    }
    return false;
}

bool Provider::get_staged_files(std::vector<ChangedFile>& out) const {
    std::string output;
    if (!run_git({"diff", "--cached", "--name-status", "--no-renames"}, output)) {
        return false;
    }
    return parse_name_status(output, out);
}

bool Provider::get_wip_files(std::vector<ChangedFile>& out) const {
    std::string output;
    if (!run_git({"diff", "HEAD", "--name-status", "--no-renames"}, output)) {
        // If HEAD doesn't exist (new repo), fall back to staged
        return get_staged_files(out);
    }
    return parse_name_status(output, out);
}

bool Provider::get_commit_files(std::string_view commit_ref,
                                std::vector<ChangedFile>& out) const {
    std::string ref(commit_ref);
    if (ref.empty()) ref = "HEAD";

    std::string output;
    if (!run_git({"diff-tree", "--no-commit-id", "--name-status", "-r", ref}, output)) {
        return false;
    }
    return parse_name_status(output, out);
}

bool Provider::get_range_files(std::string_view base_ref, std::string_view target_ref,
                               std::vector<ChangedFile>& out) const {
    if (base_ref.empty()) return false;
    std::string target(target_ref);
    if (target.empty()) target = "HEAD";

    std::string range_spec = std::string(base_ref) + ".." + target;
    std::string output;
    if (!run_git({"diff", "--name-status", "--no-renames", range_spec}, output)) {
        return false;
    }
    return parse_name_status(output, out);
}

bool Provider::parse_name_status(std::string_view output,
                                 std::vector<ChangedFile>& out) const {
    std::vector<std::string_view> lines;
    split_lines(output, lines);

    for (const auto& line : lines) {
        std::vector<std::string_view> parts;
        split_fields(line, parts);
        if (parts.size() < 2) continue;

        auto status = parts[0];
        auto path = parts[1];
        std::string old_path;

        if (parts.size() >= 3 && (status[0] == 'R' || status[0] == 'C')) {
            old_path = std::string(parts[1]);
            path = parts[2];
        }

        ChangedFile file;
        file.path = std::string(path);
        file.old_path = std::move(old_path);
        file.status = parse_status(status);
        out.push_back(std::move(file));
    }

    return true;
}

FileChangeStatus Provider::parse_status(std::string_view status) const {
    if (status.empty()) return FileChangeStatus::Modified;
    switch (status[0]) {
        case 'A': return FileChangeStatus::Added;
        case 'D': return FileChangeStatus::Deleted;
        case 'M': return FileChangeStatus::Modified;
        case 'R': return FileChangeStatus::Renamed;
        case 'C': return FileChangeStatus::Copied;
        default: return FileChangeStatus::Modified;
    }
}

bool Provider::get_file_content(std::string_view ref, std::string_view path,
                                std::string& out) const {
    if (ref.empty() || ref == "WORKING") {
        auto full_path = std::filesystem::path(repo_root_) / std::string(path);
        std::ifstream ifs(full_path, std::ios::binary);
        if (!ifs) return false;
        std::ostringstream ss;
        ss << ifs.rdbuf();
        out = ss.str();
        return true;
    }

    if (ref == "STAGED" || ref == "INDEX") {
        return get_staged_content(path, out);
    }

    std::string spec = std::string(ref) + ":" + std::string(path);
    return run_git({"show", spec}, out);
}

bool Provider::get_staged_content(std::string_view path, std::string& out) const {
    std::string spec = ":" + std::string(path);
    return run_git({"show", spec}, out);
}

bool Provider::get_diff_stats(const AnalysisParams& params, DiffStats& out) const {
    std::vector<std::string> args;

    switch (params.scope) {
        case AnalysisScope::Staged:
            args = {"diff", "--cached", "--numstat"};
            break;
        case AnalysisScope::WIP:
            args = {"diff", "HEAD", "--numstat"};
            break;
        case AnalysisScope::Commit: {
            std::string ref = params.base_ref;
            if (ref.empty()) ref = "HEAD";
            args = {"diff-tree", "--no-commit-id", "--numstat", "-r", ref};
            break;
        }
        case AnalysisScope::Range: {
            if (params.base_ref.empty()) return false;
            std::string target = params.target_ref;
            if (target.empty()) target = "HEAD";
            std::string range_spec = params.base_ref + ".." + target;
            args = {"diff", "--numstat", range_spec};
            break;
        }
    }

    std::string output;
    if (!run_git(args, output)) return false;
    return parse_numstat(output, out);
}

bool Provider::parse_numstat(std::string_view output, DiffStats& out) const {
    out = DiffStats{};
    std::vector<std::string_view> lines;
    split_lines(output, lines);

    for (const auto& line : lines) {
        std::vector<std::string_view> parts;
        split_fields(line, parts);
        if (parts.size() < 3) continue;

        int added = 0;
        int deleted = 0;
        // Binary files show '-' for stats
        if (parts[0] != "-") {
            auto r1 = std::from_chars(parts[0].data(), parts[0].data() + parts[0].size(), added);
            if (r1.ec != std::errc{}) added = 0;
        }
        if (parts[1] != "-") {
            auto r2 = std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), deleted);
            if (r2.ec != std::errc{}) deleted = 0;
        }

        out.total_added += added;
        out.total_deleted += deleted;

        if (added > 0 && deleted == 0) {
            out.files_added++;
        } else if (added == 0 && deleted > 0) {
            out.files_deleted++;
        } else {
            out.files_modified++;
        }
    }

    return true;
}

bool Provider::get_base_ref(const AnalysisParams& params, std::string& out) const {
    switch (params.scope) {
        case AnalysisScope::Staged:
        case AnalysisScope::WIP:
            out = "HEAD";
            return true;
        case AnalysisScope::Commit: {
            std::string ref = params.base_ref;
            if (ref.empty()) ref = "HEAD";
            return get_parent_commit(ref, out);
        }
        case AnalysisScope::Range:
            if (params.base_ref.empty()) return false;
            out = params.base_ref;
            return true;
    }
    out = "HEAD";
    return true;
}

bool Provider::get_parent_commit(std::string_view commit, std::string& out) const {
    std::string ref = std::string(commit) + "^";
    if (!run_git({"rev-parse", ref}, out)) {
        // No parent (first commit), return empty tree hash
        out = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";
        return true;
    }
    // Trim trailing whitespace
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return true;
}

std::string Provider::get_target_ref(const AnalysisParams& params) const {
    switch (params.scope) {
        case AnalysisScope::Staged: return "STAGED";
        case AnalysisScope::WIP: return "WORKING";
        case AnalysisScope::Commit:
            return params.base_ref.empty() ? "HEAD" : params.base_ref;
        case AnalysisScope::Range:
            return params.target_ref.empty() ? "HEAD" : params.target_ref;
    }
    return "HEAD";
}

bool Provider::list_all_files(std::vector<std::string>& out) const {
    std::string output;
    if (!run_git({"ls-files"}, output)) return false;

    std::vector<std::string_view> lines;
    split_lines(output, lines);
    out.reserve(lines.size());
    for (const auto& line : lines) {
        out.emplace_back(line);
    }
    return true;
}

bool Provider::get_current_branch(std::string& out) const {
    if (!run_git({"rev-parse", "--abbrev-ref", "HEAD"}, out)) return false;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return true;
}

bool Provider::get_commit_hash(std::string_view ref, std::string& out) const {
    if (!run_git({"rev-parse", std::string(ref)}, out)) return false;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return true;
}

}  // namespace git
}  // namespace lci
