#include <lci/indexing/watcher.h>

#include <filesystem>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#if defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <poll.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#error "Unsupported platform for file watching"
#endif

namespace lci {
namespace fs = std::filesystem;

namespace {

/// Returns a stable identity for a directory to detect symlink cycles.
/// Uses canonical path hashing, matching the FileScanner approach.
uint64_t dir_identity(const fs::path& p) {
    std::error_code ec;
    auto canonical = fs::canonical(p, ec);
    if (ec) return 0;
    return std::hash<std::string>{}(canonical.string());
}

}  // namespace

// -- Platform state -----------------------------------------------------------

#if defined(__linux__)

struct FileWatcher::PlatformState {
    int inotify_fd{-1};
    int shutdown_pipe[2]{-1, -1};
    absl::flat_hash_map<int, std::string> wd_to_path;

    ~PlatformState() {
        if (inotify_fd >= 0) close(inotify_fd);
        if (shutdown_pipe[0] >= 0) close(shutdown_pipe[0]);
        if (shutdown_pipe[1] >= 0) close(shutdown_pipe[1]);
    }
};

#elif defined(__APPLE__)

struct FileWatcher::PlatformState {
    FSEventStreamRef stream{nullptr};
    dispatch_queue_t queue{nullptr};
    int shutdown_pipe[2]{-1, -1};

    ~PlatformState() {
        if (stream) {
            FSEventStreamStop(stream);
            FSEventStreamInvalidate(stream);
            FSEventStreamRelease(stream);
        }
        if (queue) dispatch_release(queue);
        if (shutdown_pipe[0] >= 0) close(shutdown_pipe[0]);
        if (shutdown_pipe[1] >= 0) close(shutdown_pipe[1]);
    }
};

#elif defined(_WIN32)

struct FileWatcher::PlatformState {
    HANDLE dir_handle{INVALID_HANDLE_VALUE};
    HANDLE shutdown_event{nullptr};
    std::string root_path;

    ~PlatformState() {
        if (dir_handle != INVALID_HANDLE_VALUE) CloseHandle(dir_handle);
        if (shutdown_event != nullptr) CloseHandle(shutdown_event);
    }
};

#endif

// -- Construction / Destruction -----------------------------------------------

FileWatcher::FileWatcher(const Config& config)
    : config_(config) {
    gitignore_.load_gitignore(config.project.root);
}

FileWatcher::~FileWatcher() {
    stop();
}

void FileWatcher::set_callback(EventCallback cb) {
    callback_ = std::move(cb);
}

// -- Start / Stop -------------------------------------------------------------

bool FileWatcher::start(const std::string& root) {
    if (!config_.index.watch_mode) return false;
    if (root.empty() || !fs::exists(root)) return false;

    platform_ = std::make_unique<PlatformState>();

#if defined(__linux__)
    platform_->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (platform_->inotify_fd < 0) return false;

    if (pipe(platform_->shutdown_pipe) != 0) {
        platform_.reset();
        return false;
    }
#elif defined(__APPLE__)
    if (pipe(platform_->shutdown_pipe) != 0) {
        platform_.reset();
        return false;
    }
#elif defined(_WIN32)
    platform_->shutdown_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (platform_->shutdown_event == nullptr) {
        platform_.reset();
        return false;
    }
    platform_->root_path = root;
    platform_->dir_handle = CreateFileA(
        root.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (platform_->dir_handle == INVALID_HANDLE_VALUE) {
        platform_.reset();
        return false;
    }
#endif

    if (!add_watches(root)) {
        platform_.reset();
        return false;
    }

    running_.store(true, std::memory_order_release);
    event_thread_ = std::thread(&FileWatcher::event_loop, this);
    return true;
}

void FileWatcher::stop() {
    bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (!was_running) return;

#if defined(__linux__) || defined(__APPLE__)
    if (platform_ && platform_->shutdown_pipe[1] >= 0) {
        char buf = 1;
        auto ret = write(platform_->shutdown_pipe[1], &buf, 1);
        (void)ret;
    }
#elif defined(_WIN32)
    if (platform_ && platform_->shutdown_event != nullptr) {
        SetEvent(platform_->shutdown_event);
    }
#endif

    if (event_thread_.joinable()) {
        event_thread_.join();
    }
    platform_.reset();
}

// -- Recursive watch setup ----------------------------------------------------

bool FileWatcher::add_watches(const std::string& root) {
#if defined(__APPLE__)
    // FSEvents watches the entire tree from a single root path.
    CFStringRef path_ref = CFStringCreateWithCString(
        kCFAllocatorDefault, root.c_str(), kCFStringEncodingUTF8);
    if (!path_ref) return false;

    CFArrayRef paths = CFArrayCreate(
        kCFAllocatorDefault,
        reinterpret_cast<const void**>(&path_ref), 1,
        &kCFTypeArrayCallBacks);
    CFRelease(path_ref);
    if (!paths) return false;

    FSEventStreamContext ctx{};
    ctx.info = this;

    platform_->stream = FSEventStreamCreate(
        kCFAllocatorDefault, &fsevents_trampoline,
        &ctx, paths, kFSEventStreamEventIdSinceNow, 0.3,
        kFSEventStreamCreateFlagFileEvents |
            kFSEventStreamCreateFlagNoDefer);
    CFRelease(paths);
    if (!platform_->stream) return false;

    platform_->queue = dispatch_queue_create("lci.fswatcher", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(platform_->stream, platform_->queue);
    FSEventStreamStart(platform_->stream);
    return true;

#elif defined(_WIN32)
    // ReadDirectoryChangesW watches the root recursively.
    // Actual watching is done in the event_loop via overlapped I/O.
    (void)root;
    return platform_->dir_handle != INVALID_HANDLE_VALUE;

#else
    // Linux: add per-directory inotify watches.
    absl::flat_hash_set<uint64_t> visited_inodes;
    std::error_code ec;

    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::follow_directory_symlink |
                       fs::directory_options::skip_permission_denied,
             ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {

        if (ec) { ec.clear(); continue; }
        if (!it->is_directory(ec)) continue;

        // Cycle detection via canonical path identity
        auto dir_id = dir_identity(it->path());
        if (dir_id != 0 && visited_inodes.contains(dir_id)) {
            it.disable_recursion_pending();
            continue;
        }
        if (dir_id != 0) visited_inodes.insert(dir_id);

        std::string path_str = it->path().string();
        if (should_ignore_dir(path_str)) {
            it.disable_recursion_pending();
            continue;
        }

        int wd = inotify_add_watch(
            platform_->inotify_fd, path_str.c_str(),
            IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
        if (wd >= 0) {
            platform_->wd_to_path[wd] = path_str;
        }
    }

    // Also watch the root itself
    int wd = inotify_add_watch(
        platform_->inotify_fd, root.c_str(),
        IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    if (wd >= 0) {
        platform_->wd_to_path[wd] = root;
    }

    return true;
#endif
}

// -- Directory / path filtering -----------------------------------------------

bool FileWatcher::should_ignore_dir(const std::string& path) const {
    std::string basename = fs::path(path).filename().string();

    for (const auto& pattern : config_.exclude) {
        std::string dir_pattern = pattern;
        if (dir_pattern.ends_with("/**")) {
            dir_pattern = dir_pattern.substr(0, dir_pattern.size() - 3);
        }
        if (FileScanner::match_glob(dir_pattern, basename)) return true;
        if (FileScanner::match_glob(pattern, path)) return true;
    }

    if (config_.index.respect_gitignore) {
        std::error_code ec;
        auto rel = fs::relative(path, config_.project.root, ec);
        if (!ec) {
            std::string rel_str = rel.generic_string();
            if (gitignore_.should_ignore(rel_str, true)) return true;
        }
    }

    return false;
}

bool FileWatcher::should_process_path(const std::string& path) const {
    std::string basename = fs::path(path).filename().string();

    for (const auto& pattern : config_.exclude) {
        if (FileScanner::match_glob(pattern, basename)) return false;
        if (FileScanner::match_glob(pattern, path)) return false;
    }

    if (config_.index.respect_gitignore) {
        std::error_code ec;
        auto rel = fs::relative(path, config_.project.root, ec);
        if (!ec) {
            std::string rel_str = rel.generic_string();
            if (gitignore_.should_ignore(rel_str, false)) return false;
        }
    }

    if (!config_.include.empty()) {
        std::error_code ec;
        auto rel = fs::relative(path, config_.project.root, ec);
        std::string rel_str = ec ? basename : rel.generic_string();

        for (const auto& pattern : config_.include) {
            if (FileScanner::match_glob(pattern, rel_str)) return true;
            if (FileScanner::match_glob(pattern, basename)) return true;
        }
        return false;
    }

    return true;
}

// -- Event dispatch -----------------------------------------------------------

void FileWatcher::dispatch_event(const std::string& path,
                                 FileEventType type) {
    {
        std::lock_guard lock(stats_mu_);
        ++events_processed_;
        last_event_time_ = std::chrono::steady_clock::now();
    }
    if (callback_) {
        callback_(path, type);
    }
}

// -- Event loop (platform-specific) -------------------------------------------

#if defined(__linux__)

void FileWatcher::event_loop() {
    constexpr size_t kBufLen = 4096;
    alignas(inotify_event) char buf[kBufLen];

    pollfd fds[2]{};
    fds[0].fd = platform_->inotify_fd;
    fds[0].events = POLLIN;
    fds[1].fd = platform_->shutdown_pipe[0];
    fds[1].events = POLLIN;

    while (running_.load(std::memory_order_relaxed)) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            std::lock_guard lock(stats_mu_);
            ++error_count_;
            break;
        }

        if (fds[1].revents & POLLIN) break;  // shutdown signal

        if (!(fds[0].revents & POLLIN)) continue;

        ssize_t len = read(platform_->inotify_fd, buf, kBufLen);
        if (len <= 0) continue;

        for (char* ptr = buf; ptr < buf + len;) {
            auto* event = reinterpret_cast<inotify_event*>(ptr);

            if (event->len > 0) {
                auto it = platform_->wd_to_path.find(event->wd);
                if (it != platform_->wd_to_path.end()) {
                    std::string full_path =
                        it->second + "/" + std::string(event->name);

                    if (event->mask & IN_ISDIR) {
                        // New directory created -- add watch
                        if (event->mask & IN_CREATE) {
                            if (!should_ignore_dir(full_path)) {
                                int wd = inotify_add_watch(
                                    platform_->inotify_fd, full_path.c_str(),
                                    IN_CREATE | IN_MODIFY | IN_DELETE |
                                        IN_MOVED_FROM | IN_MOVED_TO);
                                if (wd >= 0) {
                                    platform_->wd_to_path[wd] = full_path;
                                }
                            }
                        }
                    } else {
                        // File event
                        std::error_code ec;
                        auto file_size = fs::file_size(full_path, ec);
                        bool size_ok =
                            ec ||
                            file_size <=
                                static_cast<uintmax_t>(
                                    config_.index.max_file_size);

                        if (size_ok && should_process_path(full_path)) {
                            FileEventType type{};
                            if (event->mask & IN_CREATE)
                                type = FileEventType::Create;
                            else if (event->mask & IN_MODIFY)
                                type = FileEventType::Write;
                            else if (event->mask & IN_DELETE)
                                type = FileEventType::Remove;
                            else if (event->mask &
                                     (IN_MOVED_FROM | IN_MOVED_TO))
                                type = FileEventType::Rename;
                            else {
                                ptr += sizeof(inotify_event) + event->len;
                                continue;
                            }
                            dispatch_event(full_path, type);
                        }
                    }
                }
            }

            ptr += sizeof(inotify_event) + event->len;
        }
    }
}

#elif defined(__APPLE__)

static void fsevents_trampoline(
    ConstFSEventStreamRef /*stream*/, void* context, size_t num_events,
    void* event_paths, const FSEventStreamEventFlags event_flags[],
    const FSEventStreamEventId /*event_ids*/[]) {
    auto* watcher = static_cast<FileWatcher*>(context);
    watcher->handle_fsevents(num_events, event_paths, event_flags);
}

void FileWatcher::handle_fsevents(size_t num_events, void* event_paths,
                                  const unsigned int event_flags[]) {
    auto** paths = static_cast<char**>(event_paths);

    for (size_t i = 0; i < num_events; ++i) {
        std::string path(paths[i]);

        if (event_flags[i] & kFSEventStreamEventFlagItemIsDir) {
            if (should_ignore_dir(path)) continue;
        } else {
            if (!should_process_path(path)) continue;

            std::error_code ec;
            auto file_size = fs::file_size(path, ec);
            if (!ec && file_size > static_cast<uintmax_t>(
                           config_.index.max_file_size)) {
                continue;
            }

            FileEventType type{};
            if (event_flags[i] & kFSEventStreamEventFlagItemCreated)
                type = FileEventType::Create;
            else if (event_flags[i] & kFSEventStreamEventFlagItemModified)
                type = FileEventType::Write;
            else if (event_flags[i] & kFSEventStreamEventFlagItemRemoved)
                type = FileEventType::Remove;
            else if (event_flags[i] & kFSEventStreamEventFlagItemRenamed)
                type = FileEventType::Rename;
            else
                continue;

            dispatch_event(path, type);
        }
    }
}

void FileWatcher::event_loop() {
    // FSEvents dispatches callbacks on platform_->queue.
    // This thread simply waits for the shutdown signal.
    pollfd fds[1]{};
    fds[0].fd = platform_->shutdown_pipe[0];
    fds[0].events = POLLIN;

    while (running_.load(std::memory_order_relaxed)) {
        int ret = poll(fds, 1, 1000);
        if (ret < 0 && errno != EINTR) break;
        if (ret > 0 && (fds[0].revents & POLLIN)) break;
    }
}

#elif defined(_WIN32)

void FileWatcher::event_loop() {
    constexpr DWORD kBufLen = 8192;
    std::vector<BYTE> buf(kBufLen);
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) return;

    HANDLE wait_handles[2] = {overlapped.hEvent, platform_->shutdown_event};

    while (running_.load(std::memory_order_relaxed)) {
        ResetEvent(overlapped.hEvent);
        DWORD bytes_returned = 0;
        BOOL ok = ReadDirectoryChangesW(
            platform_->dir_handle, buf.data(), kBufLen, TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
            &bytes_returned, &overlapped, nullptr);

        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            std::lock_guard lock(stats_mu_);
            ++error_count_;
            break;
        }

        DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
        if (wait_result == WAIT_OBJECT_0 + 1) break;  // shutdown
        if (wait_result != WAIT_OBJECT_0) {
            std::lock_guard lock(stats_mu_);
            ++error_count_;
            break;
        }

        if (!GetOverlappedResult(platform_->dir_handle, &overlapped,
                                 &bytes_returned, FALSE)) {
            continue;
        }
        if (bytes_returned == 0) continue;

        auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf.data());
        for (;;) {
            std::wstring wname(info->FileName,
                               info->FileNameLength / sizeof(WCHAR));
            int sz = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(),
                                         static_cast<int>(wname.size()),
                                         nullptr, 0, nullptr, nullptr);
            std::string name(static_cast<size_t>(sz), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wname.c_str(),
                                static_cast<int>(wname.size()),
                                name.data(), sz, nullptr, nullptr);

            std::string full_path = platform_->root_path + "\\" + name;
            // Normalize separators
            for (char& c : full_path) {
                if (c == '\\') c = '/';
            }

            if (should_process_path(full_path)) {
                std::error_code ec;
                auto file_size = fs::file_size(full_path, ec);
                bool size_ok =
                    ec || file_size <= static_cast<uintmax_t>(
                               config_.index.max_file_size);

                if (size_ok) {
                    FileEventType type{};
                    switch (info->Action) {
                        case FILE_ACTION_ADDED:
                            type = FileEventType::Create; break;
                        case FILE_ACTION_MODIFIED:
                            type = FileEventType::Write; break;
                        case FILE_ACTION_REMOVED:
                            type = FileEventType::Remove; break;
                        case FILE_ACTION_RENAMED_OLD_NAME:
                        case FILE_ACTION_RENAMED_NEW_NAME:
                            type = FileEventType::Rename; break;
                        default:
                            goto next_entry;
                    }
                    dispatch_event(full_path, type);
                }
            }

        next_entry:
            if (info->NextEntryOffset == 0) break;
            info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                reinterpret_cast<BYTE*>(info) + info->NextEntryOffset);
        }
    }

    CloseHandle(overlapped.hEvent);
}

#endif

// -- Stats --------------------------------------------------------------------

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
