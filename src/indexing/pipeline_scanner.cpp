#include <lci/indexing/pipeline_scanner.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>

namespace lci {

namespace {

/// Returns the inode (or file_index on Windows) for a directory entry,
/// used to detect symlink cycles.
uint64_t get_inode(const std::filesystem::path& p) {
    std::error_code ec;
    auto status = std::filesystem::symlink_status(p, ec);
    if (ec) return 0;

    // For symlinks, resolve and get the canonical target's identity
    if (std::filesystem::is_symlink(status)) {
        auto target = std::filesystem::canonical(p, ec);
        if (ec) return 0;
        // Use a hash of the canonical path as stable identity
        return std::hash<std::string>{}(target.string());
    }
    // Use hash of canonical path for directories
    auto canonical = std::filesystem::canonical(p, ec);
    if (ec) return 0;
    return std::hash<std::string>{}(canonical.string());
}

/// Returns the file extension in lower-case without the leading dot.
std::string extension_no_dot(std::string_view path) {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= path.size()) return {};
    std::string ext(path.substr(dot + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

}  // namespace

FileScanner::FileScanner(const Config& config)
    : config_(config) {
    exclusions_ = config.exclude;
    inclusions_ = config.include;

    if (config.index.respect_gitignore) {
        gitignore_parser_.load_gitignore(config.project.root);
    }
}

std::vector<FileTask> FileScanner::scan() {
    std::vector<FileTask> tasks;
    absl::flat_hash_set<uint64_t> visited_inodes;
    std::filesystem::path root(config_.project.root);

    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || ec) return tasks;

    walk_directory(root, visited_inodes, tasks);

    // Sort by priority descending so high-priority files are processed
    // first. Within the same priority bucket, sort by path ascending so
    // the order in which the integrator sees files (and therefore the
    // file_id / symbol_id assignment) is deterministic across runs.
    std::sort(tasks.begin(), tasks.end(),
              [](const FileTask& a, const FileTask& b) {
                  if (a.priority != b.priority) return a.priority > b.priority;
                  return a.path < b.path;
              });
    return tasks;
}

void FileScanner::walk_directory(
    const std::filesystem::path& dir,
    absl::flat_hash_set<uint64_t>& visited_inodes,
    std::vector<FileTask>& out) {

    // Symlink cycle detection: track visited directory identities
    uint64_t dir_id = get_inode(dir);
    if (dir_id != 0 && !visited_inodes.insert(dir_id).second) {
        return;  // Already visited this directory (symlink cycle)
    }

    std::error_code ec;
    auto iter = std::filesystem::directory_iterator(dir, ec);
    if (ec) return;

    std::filesystem::path root(config_.project.root);
    for (const auto& entry : iter) {
        auto rel = std::filesystem::relative(entry.path(), root, ec);
        if (ec) continue;
        std::string rel_path = rel.generic_string();

        if (entry.is_directory(ec)) {
            if (ec) continue;

            // Skip hidden directories
            auto dirname = entry.path().filename().string();
            if (!dirname.empty() && dirname[0] == '.') continue;

            // Check gitignore for directory
            if (config_.index.respect_gitignore &&
                gitignore_parser_.should_ignore(rel_path, true))
                continue;

            if (should_exclude(rel_path)) continue;

            // Handle symlinks to directories
            if (entry.is_symlink(ec) && !ec) {
                if (!config_.index.follow_symlinks) continue;
            }

            walk_directory(entry.path(), visited_inodes, out);
            continue;
        }

        if (!entry.is_regular_file(ec) || ec) continue;

        auto file_size = static_cast<int64_t>(entry.file_size(ec));
        if (ec) continue;

        if (!should_process_file(entry.path(), rel_path, file_size))
            continue;

        FileTask task;
        task.path = entry.path().string();
        task.language = detect_language(task.path);
        task.size = file_size;
        task.priority = get_file_priority(task.path);
        out.push_back(std::move(task));
    }
}

bool FileScanner::should_exclude(std::string_view rel_path) const {
    for (const auto& pattern : exclusions_) {
        if (match_glob(pattern, rel_path)) return true;
    }
    return false;
}

bool FileScanner::should_include(std::string_view rel_path) const {
    if (inclusions_.empty()) return true;
    for (const auto& pattern : inclusions_) {
        if (match_glob(pattern, rel_path)) return true;
    }
    return false;
}

bool FileScanner::should_process_file(
    const std::filesystem::path& path,
    std::string_view rel_path,
    int64_t file_size) const {

    if (binary_detector_.is_binary_by_extension(path.string())) return false;

    if (config_.index.respect_gitignore &&
        gitignore_parser_.should_ignore(rel_path, false))
        return false;

    if (should_exclude(rel_path)) return false;
    if (!should_include(rel_path)) return false;

    if (file_size > config_.index.max_file_size) return false;

    return true;
}

int FileScanner::get_file_priority(std::string_view path) {
    auto ext = extension_no_dot(path);
    if (ext == "go" || ext == "rs" || ext == "py" || ext == "js" || ext == "ts")
        return 10;
    if (ext == "java" || ext == "cpp" || ext == "c" || ext == "h")
        return 8;
    if (ext == "md" || ext == "txt" || ext == "yaml" || ext == "yml" ||
        ext == "json")
        return 5;
    return 1;
}

std::string FileScanner::detect_language(std::string_view path) {
    auto ext = extension_no_dot(path);
    if (ext == "go") return "go";
    if (ext == "py") return "python";
    if (ext == "js") return "javascript";
    if (ext == "ts") return "typescript";
    if (ext == "tsx") return "tsx";
    if (ext == "jsx") return "javascript";
    if (ext == "rs") return "rust";
    if (ext == "c" || ext == "h") return "c";
    if (ext == "cpp" || ext == "cxx" || ext == "cc" || ext == "hpp" ||
        ext == "hxx")
        return "cpp";
    if (ext == "java") return "java";
    if (ext == "cs") return "c_sharp";
    if (ext == "php") return "php";
    if (ext == "kt" || ext == "kts") return "kotlin";
    if (ext == "zig") return "zig";
    if (ext == "rb") return "ruby";
    return "unknown";
}

bool FileScanner::match_glob(std::string_view pattern,
                             std::string_view path) {
    size_t px = 0, tx = 0;
    size_t star_px = std::string_view::npos;
    size_t star_tx = 0;

    while (tx < path.size()) {
        if (px < pattern.size() && pattern[px] == '*') {
            if (px + 1 < pattern.size() && pattern[px + 1] == '*') {
                px += 2;
                if (px < pattern.size() && pattern[px] == '/') ++px;
                star_px = px;
                star_tx = tx;
                continue;
            }
            star_px = px + 1;
            star_tx = tx;
            ++px;
            continue;
        }
        if (px < pattern.size() && pattern[px] == '?') {
            if (path[tx] != '/') { ++px; ++tx; continue; }
        } else if (px < pattern.size() && pattern[px] == path[tx]) {
            ++px; ++tx; continue;
        }
        if (star_px != std::string_view::npos) {
            px = star_px;
            ++star_tx;
            tx = star_tx;
            continue;
        }
        return false;
    }
    while (px < pattern.size() && pattern[px] == '*') ++px;
    return px == pattern.size();
}

// -- pipeline_types free functions -------------------------------------------

std::pair<int, int> calculate_optimal_channel_buffers(int file_count) {
    int cpu_count = static_cast<int>(std::thread::hardware_concurrency());
    if (cpu_count < 1) cpu_count = 4;

    int task_buf = std::max(cpu_count * pipeline_constants::kTaskChannelBaseMultiplier,
                            file_count / 20);
    if (task_buf > pipeline_constants::kMaxTaskChannelBuffer)
        task_buf = pipeline_constants::kMaxTaskChannelBuffer;

    int result_buf = std::max(
        cpu_count * pipeline_constants::kResultChannelBaseMultiplier,
        file_count / 10);
    if (result_buf > pipeline_constants::kMaxResultChannelBuffer)
        result_buf = pipeline_constants::kMaxResultChannelBuffer;

    return {task_buf, result_buf};
}

}  // namespace lci
