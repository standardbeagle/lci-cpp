#include <lci/indexing/watcher.h>

#include <filesystem>
#include <string>

#include <efsw/efsw.hpp>

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
                          std::string /*old_filename*/) override {
        if (!owner_) return;
        FileEventType type{};
        if (!map_action(action, type)) return;
        owner_->on_efsw_event(dir, filename, type);
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

FileWatcher::FileWatcher(const Config& config) : config_(config) {
    gitignore_.load_gitignore(config.project.root);
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

bool FileWatcher::should_ignore_dir(const std::string& path) const {
    std::string basename = fs::path(path).filename().string();

    std::error_code rel_ec;
    auto rel = fs::relative(path, config_.project.root, rel_ec);
    std::string rel_str = rel_ec ? basename : rel.generic_string();

    for (const auto& pattern : config_.exclude) {
        std::string dir_pattern = pattern;
        if (dir_pattern.ends_with("/**")) {
            dir_pattern = dir_pattern.substr(0, dir_pattern.size() - 3);
        }
        if (FileScanner::match_glob(dir_pattern, basename)) return true;
        if (FileScanner::match_glob(pattern, rel_str)) return true;
    }

    if (config_.index.respect_gitignore) {
        if (gitignore_.should_ignore(rel_str, true)) return true;
    }

    return false;
}

bool FileWatcher::should_process_path(const std::string& path) const {
    std::string basename = fs::path(path).filename().string();

    std::error_code rel_ec;
    auto rel = fs::relative(path, config_.project.root, rel_ec);
    std::string rel_str = rel_ec ? basename : rel.generic_string();

    for (const auto& pattern : config_.exclude) {
        if (FileScanner::match_glob(pattern, basename)) return false;
        if (FileScanner::match_glob(pattern, rel_str)) return false;
    }

    if (config_.index.respect_gitignore) {
        if (gitignore_.should_ignore(rel_str, false)) return false;
    }

    if (!config_.include.empty()) {
        for (const auto& pattern : config_.include) {
            if (FileScanner::match_glob(pattern, rel_str)) return true;
            if (FileScanner::match_glob(pattern, basename)) return true;
        }
        return false;
    }

    return true;
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
                                FileEventType type) {
    // efsw delivers `dir` with a trailing separator on POSIX; concat is safe.
    std::string full_path;
    full_path.reserve(dir.size() + filename.size() + 1);
    full_path.append(dir);
    if (!full_path.empty() && full_path.back() != '/' &&
        full_path.back() != '\\') {
        full_path.push_back('/');
    }
    full_path.append(filename);

    // Filter directory events using the same exclude/gitignore rules so
    // generated/build/.git output never propagates.  efsw doesn't separate
    // dir vs file in the Add/Delete actions, so check the filesystem.
    std::error_code ec;
    bool is_dir = fs::is_directory(full_path, ec);
    if (!ec && is_dir) {
        if (should_ignore_dir(full_path)) return;
        // Directory create/delete events are not propagated to callers — the
        // prior implementation only fired on file events.  Preserve that
        // contract.
        return;
    }

    if (!should_process_path(full_path)) return;

    // Trust the filesystem over the backend's event label: a path that no
    // longer exists is a removal. macOS FSEvents coalesces events and can hand
    // us a Write/Create-labelled event for a file that was ultimately deleted,
    // so the Remove would never surface otherwise. No-op on Linux/Windows,
    // whose backends already label deletes (gone file -> Remove stays Remove).
    if (type != FileEventType::Remove) {
        std::error_code exist_ec;
        if (!fs::exists(full_path, exist_ec)) {
            type = FileEventType::Remove;
        }
    }

    // Size cap: skip files exceeding max_file_size to match the prior
    // behaviour (avoids re-indexing huge generated artefacts). A removed file
    // has no size (file_size sets ec), so removals are never skipped here.
    if (type != FileEventType::Remove) {
        auto file_size = fs::file_size(full_path, ec);
        if (!ec && file_size >
                       static_cast<uintmax_t>(config_.index.max_file_size)) {
            return;
        }
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

}  // namespace lci
