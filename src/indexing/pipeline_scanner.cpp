#include <lci/indexing/pipeline_scanner.h>
#include <lci/indexing/generated_artifacts.h>

#include <lci/language_map.h>

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
    : config_(config),
      attr_registry_(PathAttrRegistry::with_config(config.attribute_defs,
                                                   config.attributes,
                                                   attr_error_)),
      attr_classifier_(attr_registry_) {
    for (const auto& glob : config.exclude) {
        exclusions_.push_back(compile_glob(glob));
    }
    for (const auto& glob : config.include) {
        inclusions_.push_back(compile_glob(glob));
    }

    // Project manifests name their own generated-output dirs (tsconfig
    // outDir, composer vendor-dir, csproj OutputPath) — read them so
    // non-default layouts are excluded without per-project config.
    for (auto& glob : derive_generated_excludes(config.project.root)) {
        exclusions_.push_back(compile_glob(std::move(glob)));
    }

    if (config.index.respect_gitignore) {
        gitignore_parser_.load_gitignore(config.project.root);
    }
}

ScanResult FileScanner::scan(bool apply_budget) {
    ScanResult result;
    auto& tasks = result.tasks;
    absl::flat_hash_set<uint64_t> visited_inodes;
    std::filesystem::path root(config_.project.root);

    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || ec) return result;

    walk_directory(root, "", visited_inodes, tasks);

    // Sort by priority descending so high-priority files are processed
    // first. Within the same priority bucket, sort by path ascending so
    // the order in which the integrator sees files (and therefore the
    // file_id / symbol_id assignment) is deterministic across runs.
    std::sort(tasks.begin(), tasks.end(),
              [](const FileTask& a, const FileTask& b) {
                  if (a.priority != b.priority) return a.priority > b.priority;
                  return a.path < b.path;
              });

    if (!apply_budget) return result;

    // Corpus budget: the priority sort above means the budget is spent on
    // the most valuable files first, and the cut point is deterministic.
    const int64_t byte_budget =
        config_.index.max_total_size_mb * 1024 * 1024;
    const size_t file_budget =
        static_cast<size_t>(config_.index.max_file_count);

    int64_t running_bytes = 0;
    size_t cut = tasks.size();
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (i >= file_budget || running_bytes + tasks[i].size > byte_budget) {
            cut = i;
            break;
        }
        running_bytes += tasks[i].size;
    }
    if (cut == tasks.size()) return result;  // within budget

    int64_t over_bytes = 0;
    for (size_t i = cut; i < tasks.size(); ++i) over_bytes += tasks[i].size;

    if (config_.index.overflow_policy == "reject") {
        result.error =
            "corpus exceeds the indexing budget: " +
            std::to_string(tasks.size()) + " files / " +
            std::to_string((running_bytes + over_bytes) / (1024 * 1024)) +
            " MB vs index.max_file_count=" +
            std::to_string(config_.index.max_file_count) +
            ", index.max_total_size_mb=" +
            std::to_string(config_.index.max_total_size_mb) +
            " (raise the limits, tighten exclude patterns, or set "
            "index.overflow_policy \"reduced\")";
        tasks.clear();
        return result;
    }

    result.skipped_files = static_cast<int>(tasks.size() - cut);
    result.skipped_bytes = over_bytes;
    tasks.resize(cut);
    return result;
}

void FileScanner::walk_directory(
    const std::filesystem::path& dir,
    const std::string& rel_prefix,
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

    for (const auto& entry : iter) {
        std::string name = entry.path().filename().generic_string();
        if (name.empty()) continue;
        std::string rel_path =
            rel_prefix.empty() ? name : rel_prefix + '/' + name;

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

            walk_directory(entry.path(), rel_path, visited_inodes, out);
            continue;
        }

        if (!entry.is_regular_file(ec) || ec) continue;

        auto file_size = static_cast<int64_t>(entry.file_size(ec));
        if (ec) continue;

        if (!should_process_file(entry.path(), rel_path, file_size))
            continue;

        FileTask task;
        // generic_string(), not string(): file-map keys are matched by exact
        // string equality (MasterIndex::path_to_id) and joined with literal
        // '/' elsewhere, so keys must be forward-slash on every platform.
        // On Windows string() yields backslashes that never match a '/'-built
        // lookup; on POSIX the two are identical, so this is a no-op there.
        // Also harmonizes with the incremental watcher, which already stores
        // generic_string().
        task.path = entry.path().generic_string();
        task.language = detect_language(task.path);
        task.size = file_size;
        task.priority = get_file_priority(task.path);
        out.push_back(std::move(task));
    }
}

FileScanner::CompiledGlob FileScanner::compile_glob(std::string pattern) {
    // A bare pattern with no '/' names FILES, not root-level entries:
    // include { "*.zig" } means every .zig file in the tree, exactly like
    // .gitignore's basename semantics. Without this, "*.zig" matched only
    // root.zig and silently dropped sub/nested.zig. `**/` collapses to
    // nothing (see below), so root-level files still match.
    if (pattern.find('/') == std::string::npos) {
        pattern.insert(0, "**/");
    }
    // Longest wildcard-free run: every path the glob matches contains it
    // verbatim, so its absence is a certain non-match.
    std::string best;
    std::string current;
    for (char c : pattern) {
        if (c == '*' || c == '?') {
            if (current.size() > best.size()) best = current;
            current.clear();
        } else {
            current += c;
        }
    }
    if (current.size() > best.size()) best = std::move(current);
    // `**/` collapses to nothing ("**/node_modules/**" matches a top-level
    // "node_modules/x"), so a boundary slash next to a wildcard is not
    // guaranteed to appear verbatim — trim slashes from the literal's ends.
    while (!best.empty() && best.front() == '/') best.erase(best.begin());
    while (!best.empty() && best.back() == '/') best.pop_back();
    // Short literals reject nothing worth the find() call.
    if (best.size() < 3) best.clear();
    return {std::move(pattern), std::move(best)};
}

bool FileScanner::matches_compiled(const CompiledGlob& glob,
                                   std::string_view rel_path) {
    if (!glob.literal.empty() &&
        rel_path.find(glob.literal) == std::string_view::npos) {
        return false;
    }
    return match_glob(glob.pattern, rel_path);
}

bool FileScanner::should_exclude(std::string_view rel_path) const {
    for (const auto& pattern : exclusions_) {
        if (matches_compiled(pattern, rel_path)) return true;
    }
    return false;
}

bool FileScanner::should_include(std::string_view rel_path) const {
    if (inclusions_.empty()) return true;
    for (const auto& pattern : inclusions_) {
        if (matches_compiled(pattern, rel_path)) return true;
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

    // Attribute gate, path-only (there is no content yet): an attribute that
    // does not activate Index keeps its files out of the index entirely.
    // Every shipped attribute activates it, so a default corpus is scanned
    // exactly as before.
    if (!attr_registry_.activates(attr_classifier_.classify(rel_path),
                                  Capability::Index)) {
        return false;
    }

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
    // Route through the central language_map (include/lci/language_map.h) so
    // this site cannot drift from every other classification site the way the
    // old hard-coded switch did: it emitted "tsx"/"c_sharp" naming divergences,
    // mapped .h to "c", and dropped the ESM/CJS variants. language_info_for_path
    // + to_string(LangId) yield the canonical, index-wide names and pick up the
    // full extension set (.mjs/.cjs/.mts/.cts, .h -> "cpp") for free. Unknown
    // extensions resolve to LangId::Unknown -> "unknown" (no silent fallback to
    // a wrong language).
    return std::string(to_string(language_info_for_path(path).language));
}

namespace {

// Recursive glob matcher with proper `/` boundary handling.
//   `?` matches any single non-`/` char
//   `*` matches zero or more non-`/` chars (component-local)
//   `**` matches zero or more chars across boundaries (any subtree)
//   `**/` is an anchored form: the suffix that follows must start at a
//        path-component boundary (start-of-string or just after a `/`).
//   anything else is a literal.
bool match_glob_at(std::string_view pattern, size_t px,
                   std::string_view path, size_t tx) {
    while (px < pattern.size()) {
        char c = pattern[px];
        if (c == '*') {
            bool double_star =
                (px + 1 < pattern.size() && pattern[px + 1] == '*');
            if (double_star) {
                size_t next_px = px + 2;
                bool slash_anchored = false;
                if (next_px < pattern.size() && pattern[next_px] == '/') {
                    ++next_px;
                    slash_anchored = true;
                }
                // `**/X` requires the tail X to start at a component
                // boundary. Plain `**X` allows any position.
                for (size_t end = tx; end <= path.size(); ++end) {
                    if (slash_anchored && end != 0 &&
                        !(end <= path.size() && path[end - 1] == '/')) {
                        continue;
                    }
                    if (match_glob_at(pattern, next_px, path, end)) return true;
                }
                return false;
            }
            // Single `*`: match zero or more non-`/` chars then continue.
            size_t next_px = px + 1;
            for (size_t end = tx;; ++end) {
                if (match_glob_at(pattern, next_px, path, end)) return true;
                if (end >= path.size() || path[end] == '/') break;
            }
            return false;
        }
        if (c == '?') {
            if (tx >= path.size() || path[tx] == '/') return false;
            ++px; ++tx;
            continue;
        }
        if (tx >= path.size() || c != path[tx]) return false;
        ++px; ++tx;
    }
    return tx == path.size();
}

}  // namespace

bool FileScanner::match_glob(std::string_view pattern,
                             std::string_view path) {
    return match_glob_at(pattern, 0, path, 0);
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
