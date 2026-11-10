#include <lci/indexing/watcher.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include <efsw/efsw.hpp>

#include <lci/indexing/master_index.h>

namespace lci {
namespace fs = std::filesystem;

namespace {

/// Map an efsw action onto the watcher's event type.  Returns std::nullopt
/// shape via a sentinel since we want to drop unsupported actions silently.
bool map_action(efsw::Action action, FileEventType& out) {
    switch (action) {
        case efsw::Actions::Add:      out = FileEventType::Create; return true;
        case efsw::Actions::Modified: out = FileEventType::Write;  return true;
        case efsw::Actions::Delete:   out = FileEventType::Remove; return true;
        case efsw::Actions::Moved:    out = FileEventType::Rename; return true;
        default:                                                    return false;
    }
}

/// Bridge between efsw's listener interface and FileWatcher.  efsw owns the
/// listener instance via shared ownership semantics, so we keep it alive in
/// WatcherState for the duration of the watch.
class EfswBridge final : public efsw::FileWatchListener {
  public:
    explicit EfswBridge(FileWatcher* owner) : owner_(owner) {}

    void handleFileAction(efsw::WatchID /*wid*/, const std::string& dir,
                          const std::string& filename, efsw::Action action,
                          std::string old_filename) override {
        if (!owner_) return;
        FileEventType type{};
        if (!map_action(action, type)) return;
        owner_->on_efsw_event(dir, filename, type, old_filename);
    }

  private:
    FileWatcher* owner_;  // Non-owning; outlived by WatcherState (and thus owner)
};

}  // namespace

// -- Watcher state -----------------------------------------------------------

struct FileWatcher::WatcherState {
    // Listener is constructed before watcher and destroyed after — ensures
    // efsw never dispatches into a freed bridge.  efsw::FileWatcher's dtor
    // joins its worker thread, then we drop the listener.
    EfswBridge listener;
    efsw::FileWatcher watcher;  // RAII: dtor stops worker + releases handles.
    efsw::WatchID watch_id{0};

    explicit WatcherState(FileWatcher* owner)
        : listener(owner), watcher() {}
};

// -- Construction / Destruction ----------------------------------------------

FileWatcher::FileWatcher(const Config& config)
    : config_(config), scanner_(config) {
    std::error_code ec;
    const fs::path resolved = fs::canonical(config.project.root, ec);
    if (!ec && resolved != fs::path(config.project.root).lexically_normal()) {
        resolved_root_ = resolved.string();
    }
}

FileWatcher::~FileWatcher() {
    stop();
}

void FileWatcher::set_callback(EventCallback cb) {
    callback_ = std::move(cb);
}

// -- Start / Stop ------------------------------------------------------------

bool FileWatcher::start(const std::string& root) {
    if (!config_.index.watch_mode) return false;
    if (root.empty()) return false;

    std::error_code ec;
    if (!fs::exists(root, ec) || ec) return false;

    state_ = std::make_unique<WatcherState>(this);

    // recursive=true so subdirectories are auto-watched as they appear.
    state_->watch_id = state_->watcher.addWatch(root, &state_->listener, true);
    if (state_->watch_id < 0) {
        // efsw returns a negative WatchID on failure (e.g. inotify exhausted,
        // bad path, permission denied).  Surface as a clean failure rather
        // than a half-initialised watcher.
        state_.reset();
        return false;
    }

    state_->watcher.watch();  // Starts the worker thread.
    running_.store(true, std::memory_order_release);
    return true;
}

void FileWatcher::stop() {
    bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (!was_running) return;

    // Destroying state_ joins efsw's worker thread and releases all kernel
    // watch handles (inotify fds / FSEvents stream / ReadDirectoryChangesW
    // handle).  Listener outlives the worker thread because both live in the
    // same struct and the watcher member is destroyed first (declared first).
    state_.reset();
}

// -- Directory / path filtering ----------------------------------------------

// Both predicates delegate to the FileScanner a full scan uses, so an
// incremental event is admitted exactly when a rescan would index the path:
// one rule set (hidden dirs, gitignore, excludes including manifest-derived
// ones, includes, binary extensions, attributes), never a second copy.

std::string FileWatcher::root_relative(const std::string& path) const {
    std::error_code rel_ec;
    auto rel = fs::relative(path, config_.project.root, rel_ec);
    return rel_ec ? fs::path(path).filename().generic_string()
                  : rel.generic_string();
}

bool FileWatcher::should_ignore_dir(const std::string& path) const {
    return scanner_.directory_pruned(root_relative(path));
}

bool FileWatcher::should_process_path(const std::string& path) const {
    return scanner_.file_admitted(path, root_relative(path));
}

std::vector<std::string> FileWatcher::admitted_files_under(
    const std::string& dir) const {
    std::vector<std::string> files;
    const std::string dir_generic = fs::path(dir).generic_string();
    const std::string rel_dir = root_relative(dir);
    if (!scanner_.directory_admitted(rel_dir)) return files;
    // Children's repo-relative paths are derived from the directory's by
    // string append, not fs::relative per entry (a readlink per component).
    auto rel_of = [&](const std::string& path) {
        std::string rel = rel_dir;
        rel.append(path, dir_generic.size(), std::string::npos);
        return rel;
    };
    std::error_code ec;
    fs::recursive_directory_iterator it(
        dir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code entry_ec;
        const std::string path = it->path().generic_string();
        if (it->is_directory(entry_ec)) {
            if (entry_ec || scanner_.directory_pruned(rel_of(path))) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(entry_ec) || entry_ec) continue;
        const auto size = it->file_size(entry_ec);
        if (entry_ec ||
            size > static_cast<uintmax_t>(config_.index.max_file_size)) {
            continue;
        }
        if (!scanner_.file_admitted(it->path(), rel_of(path))) continue;
        files.push_back(path);
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string FileWatcher::under_configured_root(std::string path) const {
    if (resolved_root_.empty() || !path.starts_with(resolved_root_)) {
        return path;
    }
    const std::string_view rest =
        std::string_view(path).substr(resolved_root_.size());
    if (rest.empty() || (rest.front() != '/' && rest.front() != '\\')) {
        return path;  // A sibling such as <root>-other, not under the root.
    }
    std::string configured = config_.project.root;
    while (configured.size() > 1 &&
           (configured.back() == '/' || configured.back() == '\\')) {
        configured.pop_back();
    }
    configured.append(rest);
    return configured;
}

// -- Event dispatch ----------------------------------------------------------

void FileWatcher::dispatch_event(const std::string& path, FileEventType type) {
    {
        std::lock_guard lock(stats_mu_);
        ++events_processed_;
        last_event_time_ = std::chrono::steady_clock::now();
    }
    if (callback_) {
        callback_(path, type);
    }
}

void FileWatcher::on_efsw_event(const std::string& dir,
                                const std::string& filename,
                                FileEventType type,
                                const std::string& old_filename) {
    // A rename carries TWO facts: the old path is gone and the new one
    // exists. Dropping old_filename left the old path's index entries
    // (content, trigrams, symbols) alive forever after every rename.
    if (type == FileEventType::Rename && !old_filename.empty() &&
        old_filename != filename) {
        on_efsw_event(dir, old_filename, FileEventType::Remove);
    }

    // efsw delivers `dir` with a trailing separator on POSIX; concat is safe.
    std::string full_path;
    full_path.reserve(dir.size() + filename.size() + 1);
    full_path.append(dir);
    if (!full_path.empty() && full_path.back() != '/' &&
        full_path.back() != '\\') {
        full_path.push_back('/');
    }
    full_path.append(filename);
    full_path = under_configured_root(std::move(full_path));

    // FSEvents coalesces and does not reliably classify an unlink as a delete:
    // the event can arrive as an add or modify of a path that is already gone.
    // A file that no longer exists is removed, whatever the event called it.
    if (type == FileEventType::Create || type == FileEventType::Write) {
        std::error_code exists_ec;
        if (!fs::exists(full_path, exists_ec) && !exists_ec) {
            type = FileEventType::Remove;
        }
    }

    // Filter directory events using the same exclude/gitignore rules so
    // generated/build/.git output never propagates.  efsw doesn't separate
    // dir vs file in the Add/Delete actions, so check the filesystem.
    std::error_code ec;
    bool is_dir = fs::is_directory(full_path, ec);
    if (!ec && is_dir) {
        if (should_ignore_dir(full_path)) return;
        // A directory created or renamed into place arrives as this ONE
        // event: the kernel reports nothing for the files of a tree moved
        // in, so the pipeline indexes it from the directory. A Remove of a
        // still-present directory (efsw can deliver it early) propagates
        // too. Dispatch directly: should_process_path applies FILE include
        // patterns (e.g. *.go) that a directory path would never match. A
        // Write on a directory is a metadata change with nothing to index.
        if (type != FileEventType::Write) dispatch_event(full_path, type);
        return;
    }

    if (!should_process_path(full_path)) {
        // A gone path cannot be classified as file or directory. Failing
        // the FILE rules, it may still have been a directory holding
        // indexed files (a directory rename's old name, a deleted tree), so
        // its Remove propagates unless a scan would have pruned it as a
        // directory too.
        if (type == FileEventType::Remove &&
            scanner_.directory_admitted(root_relative(full_path))) {
            dispatch_event(full_path, type);
        }
        return;
    }

    // Size cap: skip files exceeding max_file_size to match the prior
    // behaviour (avoids re-indexing huge generated artefacts).
    auto file_size = fs::file_size(full_path, ec);
    if (!ec && file_size >
                   static_cast<uintmax_t>(config_.index.max_file_size)) {
        return;
    }

    dispatch_event(full_path, type);
}

// -- Stats -------------------------------------------------------------------

WatchStats FileWatcher::get_stats() const {
    std::lock_guard lock(stats_mu_);
    return WatchStats{
        .events_processed = events_processed_,
        .error_count = error_count_,
        .last_event_time = last_event_time_,
        .is_active = running_.load(std::memory_order_relaxed),
    };
}

// -- WatchPipeline -------------------------------------------------------------

WatchPipeline::WatchPipeline(const Config& config, MasterIndex& index)
    : config_(config),
      index_(index),
      watcher_(config),
      rebuilder_(std::chrono::milliseconds{config.index.watch_debounce_ms}) {
    watcher_.set_callback(
        [this](const std::string& path, FileEventType type) {
            on_event(path, type);
        });
    rebuilder_.set_callback(
        [this](const std::vector<FileID>& file_ids) { on_rebuild(file_ids); });
}

WatchPipeline::~WatchPipeline() {
    stop();
}

bool WatchPipeline::start() {
    if (!config_.index.watch_mode) return false;
    // A fresh watch starts from a clean deletion set — every file the
    // index currently holds is alive by construction.
    deleted_.clear();
    return watcher_.start(config_.project.root);
}

void WatchPipeline::stop() {
    // Watcher first: no new events may be scheduled once the debouncer is
    // drained. DebouncedRebuilder::shutdown joins its timer thread; the
    // FileWatcher dtor (via stop) joins efsw's worker.
    watcher_.stop();
    rebuilder_.shutdown();
}

void WatchPipeline::on_event(const std::string& path, FileEventType type) {
    // Runs on efsw's callback thread, which must never block on an index
    // write: index_file/update_file/remove_file take bulk_mu_ and so wait
    // out a whole bulk reindex window. Only the DebouncedRebuilder's timer
    // thread ever applies writes, so every event — Remove and new-path
    // included — is queued here and reconciled in on_rebuild.
    if (type == FileEventType::Remove) {
        // Drop any pending debounced rebuild for the path — the file is
        // gone, its rebuild would read nothing.
        {
            std::lock_guard lock(paths_mu_);
            // (iterator-returning erase is not available on flat_hash_map
            // in this absl version, hence erase_if over the tiny map.)
            absl::erase_if(scheduled_paths_, [&](const auto& entry) {
                return entry.second == path;
            });
            pending_paths_.insert(path);
        }
        rebuilder_.schedule_rebuild(kPathBatchId);
        return;
    }

    const FileID fid = index_.path_to_id(path);
    if (fid == FileID{0}) {
        // New path: no FileID to debounce on, and batching by id 0 would
        // collapse distinct new files into one rebuild. Queue by path.
        {
            std::lock_guard lock(paths_mu_);
            pending_paths_.insert(path);
        }
        rebuilder_.schedule_rebuild(kPathBatchId);
        return;
    }
    {
        std::lock_guard lock(paths_mu_);
        scheduled_paths_[fid] = path;
    }
    rebuilder_.schedule_rebuild(fid);
}

// Reconcile one queued path against the disk and the index. Runs on the
// debouncer timer thread, which may block on bulk_mu_.
void WatchPipeline::apply_path(const std::string& path,
                               absl::flat_hash_set<std::string>& vanished) {
    const FileID fid = index_.path_to_id(path);
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        // Removed (or vanished between event and rebuild).
        if (fid != FileID{0}) {
            deleted_.mark_deleted(fid);
            index_.remove_file(path);
        } else {
            // Not an indexed file: possibly a directory renamed away or
            // deleted as a tree, whose files get no events of their own.
            vanished.insert(path);
        }
        return;
    }
    if (fid == FileID{0} && fs::is_directory(path, ec) && !ec) {
        // A directory created or moved into place: index what a scan
        // would. Files already indexed keep their own Write events.
        for (const auto& file : watcher_.admitted_files_under(path)) {
            if (index_.path_to_id(file) == FileID{0}) index_.index_file(file);
        }
        return;
    }
    if (fid == FileID{0}) {
        // New path (or removed from the index while the event was pending):
        // fresh add.
        index_.index_file(path);
        return;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) return;
    index_.update_file(path, content);
}

void WatchPipeline::remove_files_under(
    const absl::flat_hash_set<std::string>& dirs) {
    if (dirs.empty()) return;
    std::vector<std::string> doomed;
    {
        const auto snap = index_.load_snapshot();
        for (const auto& [file, fid] : snap->file_map) {
            const std::string_view view(file);
            for (size_t i = view.rfind('/'); i != std::string_view::npos && i > 0;
                 i = view.rfind('/', i - 1)) {
                if (dirs.contains(view.substr(0, i))) {
                    doomed.push_back(file);
                    break;
                }
            }
        }
    }
    std::sort(doomed.begin(), doomed.end());
    for (const auto& file : doomed) {
        const FileID fid = index_.path_to_id(file);
        if (fid == FileID{0}) continue;
        deleted_.mark_deleted(fid);
        index_.remove_file(file);
    }
}

void WatchPipeline::on_rebuild(const std::vector<FileID>& file_ids) {
    absl::flat_hash_set<std::string> vanished;
    for (FileID fid : file_ids) {
        if (fid == kPathBatchId) {
            // Path-keyed batch: Remove and new-path events. Drain under the
            // lock, reconcile outside it.
            absl::flat_hash_set<std::string> paths;
            {
                std::lock_guard lock(paths_mu_);
                paths.swap(pending_paths_);
            }
            for (const auto& path : paths) apply_path(path, vanished);
            continue;
        }

        std::string path;
        {
            std::lock_guard lock(paths_mu_);
            auto it = scheduled_paths_.find(fid);
            if (it == scheduled_paths_.end()) continue;
            path = std::move(it->second);
            scheduled_paths_.erase(it);
        }
        apply_path(path, vanished);
    }
    remove_files_under(vanished);
}

}  // namespace lci
