#include <lci/server/server.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <thread>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include <lci/core/reference_tracker.h>
#include <lci/core/text.h>
#include <lci/pagination.h>
#include <lci/file_info.h>
#include <lci/git/analyzer.h>
#include <lci/git/provider.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/language_map.h>
#include <lci/search/search_engine.h>
#include <lci/search/search_options.h>
#include <lci/server/client.h>
#include <lci/server/request_decode.h>
#include <lci/version.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <fstream>

namespace lci {

// -- Socket path helpers ------------------------------------------------------

#ifdef _WIN32
/// On Windows, returns a loopback TCP address "127.0.0.1:<port>".
/// Port is derived deterministically from a hash to avoid collisions.
static constexpr int kWindowsBasePort = 43519;
#endif

namespace {

/// Returns a per-user identifier embedded in the socket path so two users
/// running lci against the same project (or with no project root) get
/// distinct sockets. On POSIX this is the numeric uid from getuid(); on
/// Windows there is no uid, so we hash the username (or the USERNAME env
/// var as a fallback) into the same 32-bit space the project hash uses.
uint32_t current_user_id() {
#ifdef _WIN32
    // Use USERNAME env var (set by the OS for every interactive Windows
    // session); fall back to "default" so the function never returns 0
    // for an unidentified user, which could otherwise alias with a
    // user whose hashed name happens to be 0.
    const char* user = std::getenv("USERNAME");
    if (user == nullptr || *user == '\0') {
        user = std::getenv("USER");  // MSYS / Git Bash on Windows
    }
    if (user == nullptr || *user == '\0') {
        user = "default";
    }
    uint32_t h = 2166136261u;  // FNV-1a 32-bit offset basis
    for (const char* p = user; *p != '\0'; ++p) {
        h ^= static_cast<uint32_t>(static_cast<unsigned char>(*p));
        h *= 16777619u;
    }
    return h;
#else
    return static_cast<uint32_t>(::getuid());
#endif
}

/// Own ANONYMOUS RSS in MB from /proc/self/status; -1 when unavailable
/// (non-Linux or unreadable). Used by the reaper's RSS self-cap.
/// RssAnon, not VmRSS, deliberately: since the content store retains
/// file-backed mmaps, VmRSS counts page-cache pages the kernel reclaims
/// on its own under pressure -- they cannot OOM the host, so they must
/// not trip the cap. Anonymous memory is what kills machines. Falls back
/// to VmRSS on kernels without the RssAnon field.
long read_own_rss_mb() {
#if defined(__linux__)
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) return -1;
    long anon_kb = -1;
    long rss_kb = -1;
    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::sscanf(line, "RssAnon: %ld kB", &anon_kb) == 1) continue;
        if (std::sscanf(line, "VmRSS: %ld kB", &rss_kb) == 1) continue;
    }
    std::fclose(f);
    const long kb = anon_kb >= 0 ? anon_kb : rss_kb;
    return kb < 0 ? -1 : kb / 1024;
#else
    return -1;
#endif
}

/// 32-bit polynomial hash (same algorithm previously used inline) of an
/// absolute project root path. Extracted so the default-path branch can
/// share it with the project-specific branch.
uint32_t hash_project_root(const std::string& abs_root) {
    uint32_t h = 0;
    for (char c : abs_root) {
        h = h * 31 + static_cast<uint32_t>(c);
    }
    return h;
}

uint32_t current_process_id() {
#ifdef _WIN32
    return static_cast<uint32_t>(::GetCurrentProcessId());
#else
    return static_cast<uint32_t>(::getpid());
#endif
}

}  // namespace

std::string get_socket_path() {
    const uint32_t uid = current_user_id();
#ifdef _WIN32
    // Per-user offset within the 1000-port project window so two users on
    // the same Windows host don't both bind kWindowsBasePort.
    const int port = kWindowsBasePort + static_cast<int>(uid % 1000);
    // 127.0.0.1, not "localhost": the listener binds IPv4 (server.cpp bind
    // path uses "127.0.0.1"), but "localhost" resolves to ::1 first on
    // Windows, so a localhost client hits IPv6 and gets connection-refused.
    return "127.0.0.1:" + std::to_string(port);
#else
    // Format: /<tmp>/lci-<uid>.sock — fits well under the 108-char
    // sun_path limit even with long TMPDIR settings (uid is at most 10
    // decimal digits for uint32, total fixed prefix is ~17 chars).
    char buf[64];
    std::snprintf(buf, sizeof(buf), "lci-%u.sock", uid);
    return (std::filesystem::temp_directory_path() / buf).string();
#endif
}

std::string get_socket_path_for_root(const std::string& root) {
    if (root.empty()) {
        return get_socket_path();
    }
    std::error_code ec;
    auto abs_root = std::filesystem::absolute(root, ec);
    if (ec) {
        return get_socket_path();
    }
    const std::string abs_str = abs_root.string();
    const uint32_t hash = hash_project_root(abs_str);
    const uint32_t uid = current_user_id();
#ifdef _WIN32
    // Combine uid and project hash into the per-port offset so two users
    // on the same project (or the same user across projects) both get
    // distinct ports without overflowing the 1000-slot window.
    const uint32_t mixed = (uid * 2654435761u) ^ hash;
    const int port = kWindowsBasePort + static_cast<int>(mixed % 1000);
    // 127.0.0.1, not "localhost" — see get_socket_path() for the IPv4/::1
    // rationale.
    return "127.0.0.1:" + std::to_string(port);
#else
    // Format: /<tmp>/lci-<uid>-<hash>.sock — e.g. /tmp/lci-1000-deadbeef.sock
    // Maximum length with /tmp prefix is /tmp/lci-4294967295-ffffffff.sock = 33
    // chars, well under the 108-byte sun_path limit. Even an unusually long
    // TMPDIR like /var/folders/abc/T/ leaves ~80 chars of headroom.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "lci-%u-%08x.sock", uid, hash);
    return (std::filesystem::temp_directory_path() / buf).string();
#endif
}

// -- Build ID -----------------------------------------------------------------

std::string build_id() {
    // Compile-time build ID from date and time macros.
    static const std::string id = [] {
        std::string raw = std::string(__DATE__) + " " + __TIME__;
        uint32_t h = 0;
        for (char c : raw) {
            h = h * 31 + static_cast<uint32_t>(c);
        }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08x", h);
        return std::string(buf);
    }();
    return id;
}

namespace {
std::string language_from_extension(const std::string& path) {
    auto info = language_info_for_path(path);
    if (info.language == LangId::Unknown) return "";
    return std::string(to_string(info.language));
}


}  // namespace

// -- IndexServer construction -------------------------------------------------

IndexServer::IndexServer(const Config& config)
    : config_(config),
      owned_indexer_(std::make_unique<MasterIndex>(config)),
      indexer_(owned_indexer_.get()) {}

IndexServer::IndexServer(const Config& config,
                         MasterIndex& indexer,
                         SearchEngine* search_engine)
    : config_(config),
      indexer_(&indexer),
      search_engine_(search_engine) {}

IndexServer::~IndexServer() {
    shutdown();
}

// -- Configuration ------------------------------------------------------------

void IndexServer::set_socket_path(const std::string& path) {
    socket_path_ = path;
}

std::string IndexServer::socket_path() const {
    if (!socket_path_.empty()) {
        return socket_path_;
    }
    return get_socket_path();
}

void IndexServer::set_build_id_override(const std::string& id) {
    build_id_override_ = id;
}

void IndexServer::enable_instance_registry(const std::string& dir) {
    registry_dir_ = dir;
}

void IndexServer::set_search_engine(SearchEngine* engine) {
    {
        std::unique_lock lock(mu_);
        search_engine_ = engine;
    }
    // Publishing a live engine means the external index build finished;
    // clearing one means a rebuild is in flight again.
    indexing_active_.store(engine == nullptr, std::memory_order_release);
}

bool IndexServer::is_running() const {
    return running_.load(std::memory_order_acquire);
}

// -- Server lifecycle ---------------------------------------------------------

#ifndef _WIN32
namespace {

/// True if a Unix socket path has a live listener (a bounded raw connect —
/// no Client machinery, cheap enough to run per candidate in the reaper).
bool unix_socket_alive(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return true;  // cannot tell: assume alive, never unlink
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return true;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                       sizeof(addr));
    ::close(fd);
    return rc == 0;
}

/// Removes this user's dead lci socket files (and their sidecar locks)
/// from the temp dir. Killed one-shot MCP processes leak one socket each;
/// dozens were observed accumulated. A file is dead only when nothing
/// answers a connect AND its sidecar lock is free (a starting server holds
/// the lock before it binds).
void reap_stale_sockets(const std::string& own_sock) {
    namespace fs = std::filesystem;
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "lci-%u-", current_user_id());
    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator(fs::temp_directory_path(), ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0) continue;
        if (name.size() < 5 || name.substr(name.size() - 5) != ".sock")
            continue;
        const std::string path = entry.path().string();
        if (path == own_sock) continue;
        if (unix_socket_alive(path)) continue;

        const std::string lock_path = path + ".lock";
        int lfd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC,
                         0600);
        if (lfd < 0) continue;
        if (::flock(lfd, LOCK_EX | LOCK_NB) != 0) {
            ::close(lfd);  // a starting server owns this path
            continue;
        }
        std::error_code rm_ec;
        fs::remove(path, rm_ec);
        fs::remove(lock_path, rm_ec);
        ::close(lfd);
    }
}

}  // namespace
#endif

void IndexServer::set_mcp_dispatcher(
    std::function<std::string(const std::string&)> d) {
    mcp_dispatcher_ = std::move(d);
}

void IndexServer::set_self_stop_callback(std::function<void(const char*)> cb) {
    self_stop_cb_ = std::move(cb);
}

bool IndexServer::start() {
    std::lock_guard lifecycle_lock(lifecycle_mu_);
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return false;  // already running
    }

    {
        std::lock_guard lock(shutdown_mu_);
        shutdown_requested_ = false;
    }
    shutdown_triggered_.store(false, std::memory_order_release);

    auto sock = socket_path();

#ifndef _WIN32
    // Claim the socket path FIRST via flock on a sidecar lock file. The
    // liveness probe and the unlink+bind below are not atomic: two servers
    // starting concurrently could both pass the probe and bind the same
    // path — the later bind owns the inode, the earlier keeps serving an
    // orphaned socket (three LISTENers on one path were observed live).
    // The kernel drops flock on process death, so no stale-pid handling.
    {
        const std::string lock_path = sock + ".lock";
        int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC,
                        0600);
        if (fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            // Zombie-holder grace: a live process can hold the lock while
            // serving NOTHING — its socket unlinked out from under it (the
            // stale-restart orphan class). A starting server binds within
            // moments of taking the lock, so if the socket is still dead
            // after a short grace, the holder is a zombie and refusing
            // forever turns one orphan into a root-wide outage. Proceed
            // unguarded (loudly): the zombie has no listener to orphan.
            bool holder_serves = unix_socket_alive(sock);
            if (!holder_serves) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                holder_serves = unix_socket_alive(sock);
            }
            ::close(fd);
            if (holder_serves) {
                std::fprintf(stderr,
                             "Error: another index server is starting or "
                             "already serving %s\n",
                             sock.c_str());
                running_.store(false, std::memory_order_release);
                return false;
            }
            std::fprintf(stderr,
                         "Warning: socket lock for %s is held by a process "
                         "with no listener (orphaned server); starting "
                         "unguarded\n",
                         sock.c_str());
            fd = -1;
        }
        // fd < 0 (lock file uncreatable) degrades to the probe-only guard
        // below rather than refusing to serve.
        socket_lock_fd_ = fd;
    }
#endif

    // A live listener on this address means another server already owns this
    // root (e.g. one from a build predating the sidecar lock). Unlinking/
    // rebinding would silently orphan it (its socket file vanishes while
    // the process keeps serving an unreachable inode), so refuse instead of
    // stealing the address.
    {
        Client probe(sock);
        probe.set_timeout(std::chrono::milliseconds{500});
        if (probe.is_server_running()) {
            std::fprintf(stderr,
                         "Error: another index server is already serving %s\n",
                         sock.c_str());
#ifndef _WIN32
            if (socket_lock_fd_ >= 0) {
                ::close(socket_lock_fd_);
                socket_lock_fd_ = -1;
            }
#endif
            running_.store(false, std::memory_order_release);
            return false;
        }
    }

#ifndef _WIN32
    // Remove stale socket file (Unix domain socket only), and this user's
    // other dead lci sockets while here — killed processes leak them.
    std::error_code ec;
    std::filesystem::remove(sock, ec);
    reap_stale_sockets(sock);
#endif

    if (!handlers_registered_) {
        register_handlers();
        handlers_registered_ = true;
    }
    start_time_ = std::chrono::steady_clock::now();

    // Start background indexing if we own the index. The thread is
    // tracked (not detached) so shutdown() can signal cancellation via
    // MasterIndex::request_stop() and join cleanly. Detaching here
    // would leave indexing running after the server destructor returned
    // and could touch freed members.
    //
    // The lambda mirrors handle_reindex: a cancelled run (stop
    // requested) leaves `indexing_active_` set so the superseding
    // /reindex thread is responsible for clearing it; a clean shutdown
    // clears the flag so /status reports accurately.
    if (owned_indexer_ && !search_engine_) {
        indexing_active_.store(true, std::memory_order_release);
        swap_indexing_thread(std::thread([this] {
            indexer_->index_directory(config_.project.root);

            if (indexer_->stop_requested()) {
                return;
            }
            if (!running_.load(std::memory_order_acquire)) {
                indexing_active_.store(false, std::memory_order_release);
                return;
            }

            auto engine = std::make_unique<SearchEngine>(*indexer_);
            {
                std::unique_lock engine_lock(mu_);
                owned_search_engine_ = std::move(engine);
                search_engine_ = owned_search_engine_.get();
            }
            indexing_active_.store(false, std::memory_order_release);
        }));
    } else if (search_engine_ == nullptr) {
        // Externally-owned index with no engine yet: the owner is building
        // the index and will publish via set_search_engine(). Report the
        // build as active so /status is truthful and the idle reaper does
        // not count the build as idleness.
        indexing_active_.store(true, std::memory_order_release);
    }

    // Publish the registry entry BEFORE the listener starts accepting:
    // every non-/ping request runs touch_activity, which reads
    // registry_path_, so assigning it after requests are already being
    // served is a data race on a plain std::string (UB). Publishing first
    // means handler threads only ever observe the final value. A failed
    // start removes the entry again in shutdown_locked().
    last_activity_ns_.store(
        std::chrono::steady_clock::now().time_since_epoch().count(),
        std::memory_order_release);
    publish_registry_entry();

#ifdef _WIN32
    // On Windows, use loopback TCP. Parse "127.0.0.1:<port>" from sock.
    int win_port = 0;
    auto colon_pos = sock.rfind(':');
    if (colon_pos != std::string::npos) {
        auto port = std::string_view(sock).substr(colon_pos + 1);
        const char* first = port.data();
        const char* last = first + port.size();
        auto [end, ec] = std::from_chars(first, last, win_port);
        if (ec != std::errc{} || end != last) win_port = 0;
    }
    if (win_port <= 0 || win_port > 65535) {
        running_.store(false, std::memory_order_release);
        shutdown_locked();
        return false;
    }

    listen_thread_ = std::thread([this, win_port] {
        if (!svr_.bind_to_port("127.0.0.1", win_port)) {
            running_.store(false, std::memory_order_release);
            return;
        }
        svr_.listen_after_bind();
    });

    bool ready = false;
    for (int i = 0; i < 50; ++i) {
        if (svr_.is_running()) {
            ready = true;
            break;
        }
        if (!running_.load(std::memory_order_acquire)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
#else
    // On Unix, listen on a Unix domain socket.
    svr_.set_address_family(AF_UNIX);

    listen_thread_ = std::thread([this, sock] {
        std::error_code ec2;
        std::filesystem::remove(sock, ec2);

        // For AF_UNIX, port is ignored but must be non-zero to avoid
        // getsockname() fallback in bind_internal().
        if (!svr_.bind_to_port(sock, 80)) {
            running_.store(false, std::memory_order_release);
            return;
        }

        // The socket mode is the only guard keeping other local users off
        // this server (the socket lives in a world-writable temp dir). If it
        // cannot be restricted, refuse to serve rather than silently serving
        // a user-readable socket.
        if (chmod(sock.c_str(), 0600) != 0) {
            std::fprintf(stderr,
                         "Error: cannot restrict socket permissions on %s\n",
                         sock.c_str());
            running_.store(false, std::memory_order_release);
            return;
        }

        // Record which inode we bound so shutdown only unlinks OUR socket:
        // a restart race can put a successor server's freshly bound socket
        // at the same path before this server's teardown reaches the
        // unlink.
        struct stat st{};
        if (::stat(sock.c_str(), &st) == 0) {
            bound_socket_ino_.store(static_cast<uint64_t>(st.st_ino),
                                    std::memory_order_release);
        }

        svr_.listen_after_bind();
    });

    bool ready = false;
    for (int i = 0; i < 50; ++i) {
        if (svr_.is_running()) {
            ready = true;
            break;
        }
        if (!running_.load(std::memory_order_acquire)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
#endif

    if (!ready) {
        running_.store(false, std::memory_order_release);
        shutdown_locked();
        return false;
    }

    // Lifecycle reaper: idle-exit, root-deletion exit, and (registry
    // enabled) startup eviction of least-recently-active peers. Root
    // deletion is only enforced for a root that existed when we started,
    // so a server deliberately pointed at a not-yet-created path doesn't
    // kill itself.
    std::error_code root_ec;
    const bool root_existed =
        !config_.project.root.empty() &&
        std::filesystem::exists(config_.project.root, root_ec);
    reaper_thread_ = std::thread(
        [this, root_existed] { reaper_loop(root_existed); });

    return true;
}

void IndexServer::wait() {
    std::unique_lock lock(shutdown_mu_);
    shutdown_cv_.wait(lock, [this] { return shutdown_requested_; });
}

bool IndexServer::shutdown() {
    std::lock_guard lifecycle_lock(lifecycle_mu_);
    return shutdown_locked();
}

bool IndexServer::shutdown_locked() {
    running_.store(false, std::memory_order_release);

    svr_.stop();

    if (listen_thread_.joinable()) {
        listen_thread_.join();
    }

    // Handlers are now quiesced (no new /shutdown can spawn a trigger). Join
    // the deferred-shutdown thread before tearing down the members it touches.
    if (shutdown_trigger_.joinable()) {
        shutdown_trigger_.join();
    }

    // Wake the reaper (its wait predicate observes running_ == false) and
    // join it before teardown touches members it reads.
    if (reaper_thread_.joinable()) {
        shutdown_cv_.notify_all();
        reaper_thread_.join();
    }

    // Cooperatively cancel any in-flight indexing and join its thread
    // before continuing teardown. Without this, a long-running indexing
    // run could outlive the server (use-after-free on indexer_,
    // owned_indexer_, mu_, etc.).
    cancel_indexing_thread();

#ifndef _WIN32
    // Remove the socket file (Unix domain socket only) — but only if the
    // path still holds the inode this server bound. During a stale-server
    // restart a successor can bind the same path before this teardown runs;
    // unlinking blindly would orphan the successor (running but
    // unreachable).
    {
        const std::string sock = socket_path();
        struct stat st{};
        const uint64_t bound = bound_socket_ino_.load(std::memory_order_acquire);
        if (::stat(sock.c_str(), &st) == 0 &&
            (bound == 0 || static_cast<uint64_t>(st.st_ino) == bound)) {
            std::error_code ec;
            std::filesystem::remove(sock, ec);
        }
        bound_socket_ino_.store(0, std::memory_order_release);
    }

    // Release the socket-path claim last: a successor blocked in start()'s
    // flock proceeds only once our socket file is gone. The lock file
    // itself stays (stable path; reaped with the socket when dead).
    if (socket_lock_fd_ >= 0) {
        ::close(socket_lock_fd_);
        socket_lock_fd_ = -1;
    }
#endif

    if (!registry_path_.empty()) {
        // Same successor race as the socket: the registry filename is
        // derived from the address, so a successor republishes the same
        // path. Only remove the entry if it still names this process.
        bool ours = true;
        try {
            std::ifstream in(registry_path_);
            const auto entry = nlohmann::json::parse(in);
            ours = entry.value("pid", uint32_t{0}) == current_process_id();
        } catch (const nlohmann::json::exception&) {
            // Unreadable entry is litter regardless of owner.
        }
        if (ours) {
            std::error_code reg_ec;
            std::filesystem::remove(registry_path_, reg_ec);
        }
        registry_path_.clear();
    }

    // Signal waiters
    {
        std::lock_guard lock(shutdown_mu_);
        shutdown_requested_ = true;
    }
    shutdown_cv_.notify_all();

    return true;
}

void IndexServer::cancel_indexing_thread() {
    swap_indexing_thread(std::thread{});
}

void IndexServer::swap_indexing_thread(std::thread new_thread) {
    // Atomic cancel-and-replace: serialised on `indexing_thread_mu_`
    // so two concurrent callers cannot race to overwrite a joinable
    // thread (overwriting a joinable thread calls std::terminate) and
    // so the new thread does not start its real work until the prior
    // run has been fully joined. Without joining under the lock, the
    // displaced thread keeps running concurrently with the new one and
    // both contend on `index_directory()` — the second loses the
    // is_indexing_ CAS and returns silently, leaving the index in the
    // state produced by the cancelled run.
    //
    // The lambda body does not acquire `indexing_thread_mu_`, so
    // holding it across the join cannot deadlock. Reindex requests
    // queued behind a long-running prior run wait their turn here,
    // which is the desired behaviour: the user's intent is "discard
    // the in-flight run, run again from scratch", and that requires
    // ordering.
    //
    // Shutdown safety: if `running_` is false the server is shutting
    // down (or already torn down). Refuse to install a new thread —
    // doing so would leave a joinable thread alive past `~IndexServer`
    // and trigger `std::terminate` from the member destructor. This
    // closes the race where an in-flight handler's call here lands
    // after shutdown's `cancel_indexing_thread()` has already drained
    // the slot.
    std::lock_guard<std::mutex> lock(indexing_thread_mu_);
    if (indexing_thread_.joinable()) {
        // Forward cooperative cancellation into the active pipeline
        // so the worker pool, scanner, and integrator exit at their
        // next checkpoint instead of running to completion.
        if (indexer_ != nullptr) {
            indexer_->request_stop();
        }
        indexing_thread_.join();
    }
    if (!running_.load(std::memory_order_acquire) && new_thread.joinable()) {
        // Caller raced with shutdown. Cancel and drain the
        // just-launched thread under the lock so it cannot outlive
        // the server. Joining here is safe because the lambda body
        // never acquires `indexing_thread_mu_`.
        if (indexer_ != nullptr) {
            indexer_->request_stop();
        }
        new_thread.join();
        return;
    }
    indexing_thread_ = std::move(new_thread);
}

// -- Lifecycle reaper ---------------------------------------------------------

namespace {

constexpr auto kReaperTick = std::chrono::milliseconds(500);
// Registry-file mtime is the cross-process LRU key for eviction; refreshing
// it on every request would be an fs write per request, so throttle.
constexpr int64_t kRegistryTouchIntervalNs = 60'000'000'000;  // 60s

int64_t steady_now_ns() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

}  // namespace

void IndexServer::touch_activity() {
    const int64_t now_ns = steady_now_ns();
    last_activity_ns_.store(now_ns, std::memory_order_release);

    if (registry_path_.empty()) {
        return;
    }
    int64_t last = last_registry_touch_ns_.load(std::memory_order_acquire);
    if (now_ns - last < kRegistryTouchIntervalNs) {
        return;
    }
    // CAS so concurrent handlers don't all hit the filesystem.
    if (last_registry_touch_ns_.compare_exchange_strong(
            last, now_ns, std::memory_order_acq_rel)) {
        std::error_code ec;
        std::filesystem::last_write_time(
            registry_path_, std::filesystem::file_time_type::clock::now(), ec);
        if (ec) {
            // The entry was reaped (e.g. one missed /ping during a peer scan
            // delisted us). Republish so a live server never stays a ghost;
            // write_registry_file leaves registry_path_ untouched, so this is
            // safe from concurrent handler threads.
            write_registry_file();
        }
    }
}

void IndexServer::publish_registry_entry() {
    if (registry_dir_.empty()) {
        return;
    }
    const std::string address = socket_path();
    char name[64];
    std::snprintf(name, sizeof(name), "lci-srv-%u-%08x.json",
                  current_user_id(), hash_project_root(address));
    registry_path_ =
        (std::filesystem::path(registry_dir_) / name).string();
    if (!write_registry_file()) {
        registry_path_.clear();
        return;
    }
    last_registry_touch_ns_.store(steady_now_ns(), std::memory_order_release);
}

bool IndexServer::write_registry_file() {
    if (registry_path_.empty()) {
        return false;
    }
    nlohmann::json entry{{"pid", current_process_id()},
                         {"address", socket_path()},
                         {"root", config_.project.root}};
    // Write-then-rename so peers scanning the registry never read a torn
    // entry.
    const auto tmp = registry_path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            std::fprintf(stderr,
                         "Warning: cannot write server registry entry %s\n",
                         tmp.c_str());
            return false;
        }
        out << entry.dump();
    }
    std::error_code ec;
    std::filesystem::rename(tmp, registry_path_, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

void IndexServer::evict_excess_peers() {
    const int max_instances = config_.server.max_instances;
    if (registry_dir_.empty() || max_instances <= 0) {
        return;
    }

    // Least-recently-active first, so the eviction victims are the prefix.
    auto live = list_server_instances(registry_dir_, registry_path_);

    // This server counts against the cap too (+1).
    const int excess = static_cast<int>(live.size()) + 1 - max_instances;
    if (excess <= 0) {
        return;
    }
    for (int i = 0; i < excess && i < static_cast<int>(live.size()); ++i) {
        std::string err;
        Client(live[i].address).shutdown(false, err);
        // The victim removes its own registry entry on shutdown. Eviction
        // runs only at server startup (reaper_loop's first act), so a victim
        // that ignores the request keeps its slot until some FUTURE server
        // start scans the registry and reaps the entry on ping failure.
    }
}

std::vector<ServerInstance> list_server_instances(
    const std::string& dir, const std::string& exclude_entry) {
    char prefix_buf[32];
    std::snprintf(prefix_buf, sizeof(prefix_buf), "lci-srv-%u-",
                  current_user_id());
    const std::string prefix = prefix_buf;

    std::vector<ServerInstance> live;

    std::error_code ec;
    for (const auto& dirent :
         std::filesystem::directory_iterator(dir, ec)) {
        const std::string fname = dirent.path().filename().string();
        if (fname.rfind(prefix, 0) != 0 ||
            (!exclude_entry.empty() && dirent.path() == exclude_entry) ||
            fname.size() < 6 || fname.substr(fname.size() - 5) != ".json") {
            continue;
        }
#ifndef _WIN32
        // Trust only entries this user wrote. The registry should live in a
        // 0700 per-user dir, but enumeration may be pointed at a shared dir;
        // a foreign-owned entry there is at best litter and at worst a
        // forged address steering our shutdown/eviction machinery.
        {
            struct stat entry_st{};
            if (::stat(dirent.path().c_str(), &entry_st) != 0 ||
                entry_st.st_uid != ::getuid()) {
                continue;
            }
        }
#endif
        ServerInstance inst;
        try {
            std::ifstream in(dirent.path());
            const auto entry = nlohmann::json::parse(in);
            inst.address = entry.value("address", "");
            inst.root = entry.value("root", "");
            inst.pid = entry.value("pid", uint32_t{0});
        } catch (const nlohmann::json::exception&) {
            // Torn/garbage entry: unreadable means unpingable, drop it.
        }
        if (inst.address.empty() || !Client(inst.address).is_server_running()) {
            // Dead server (crashed or reaped): its entry is registry litter.
            std::error_code rm_ec;
            std::filesystem::remove(dirent.path(), rm_ec);
            continue;
        }
        std::error_code mt_ec;
        auto mtime = std::filesystem::last_write_time(dirent.path(), mt_ec);
        if (mt_ec) {
            continue;
        }
        inst.last_activity = mtime;
        inst.entry_path = dirent.path().string();
        std::error_code root_ec;
        inst.root_exists =
            !inst.root.empty() && std::filesystem::exists(inst.root, root_ec);
        live.push_back(std::move(inst));
    }

    std::sort(live.begin(), live.end(),
              [](const ServerInstance& a, const ServerInstance& b) {
                  return a.last_activity < b.last_activity;
              });
    return live;
}

void IndexServer::request_self_stop(const char* reason) {
    std::fprintf(stderr, "Index server exiting: %s\n", reason);
    running_.store(false, std::memory_order_release);
    // Stop accepting immediately: an owner that never polls is_running()
    // (the embedded MCP server) must not keep serving as a half-dead
    // zombie after deciding to exit. Full teardown still happens in the
    // owner's shutdown(); httplib's stop() is safe from this thread and
    // idempotent with the one in shutdown_locked().
    svr_.stop();
    {
        std::lock_guard lock(shutdown_mu_);
        shutdown_requested_ = true;
    }
    shutdown_cv_.notify_all();
    // Last: an owner that cannot poll is_running() (MCP-host stdio loop)
    // gets told directly. Runs after the listener stopped so the callback
    // may exit the process without leaving a half-serving socket behind.
    if (self_stop_cb_) {
        self_stop_cb_(reason);
    }
}

void IndexServer::reaper_loop(bool root_existed_at_start) {
    // Startup eviction runs here, off the start() critical path: it pings
    // every registered peer, which is milliseconds each but unbounded in
    // count.
    evict_excess_peers();

    const auto idle_timeout =
        std::chrono::seconds(config_.server.idle_timeout_sec);

    for (;;) {
        {
            std::unique_lock lock(shutdown_mu_);
            if (shutdown_cv_.wait_for(lock, kReaperTick, [this] {
                    return shutdown_requested_ ||
                           !running_.load(std::memory_order_acquire);
                })) {
                return;
            }
        }

        if (root_existed_at_start) {
            std::error_code ec;
            if (!std::filesystem::exists(config_.project.root, ec)) {
                request_self_stop("project root deleted");
                return;
            }
        }

        // RSS self-cap (server.max_rss_mb): an index server must never be
        // the process that OOMs the host (the err-lookup incident: 26 GB
        // on a 2 GB corpus). The ladder is trim-then-exit, deliberately
        // NOT content-store shedding: clear() reuses FileIDs (index
        // corruption) and an emptied store silently un-searches every
        // file (candidates with no content are skipped, not reloaded) --
        // both worse than dying. Exit is already transparent here: the
        // client respawns the server on the next command, and a corpus
        // that genuinely exceeds the cap becomes a VISIBLE repeated-exit
        // config decision instead of silent degradation.
        if (config_.server.max_rss_mb > 0) {
            const long rss_mb = read_own_rss_mb();
            if (rss_mb > config_.server.max_rss_mb) {
#if defined(__GLIBC__)
                malloc_trim(0);  // freed arena back to the OS first
#endif
                const long after_mb = read_own_rss_mb();
                if (after_mb > config_.server.max_rss_mb) {
                    std::fprintf(stderr,
                                 "lci-server: RSS %ld MB exceeds "
                                 "server.max_rss_mb %d MB after trim -- "
                                 "exiting rather than risking the host "
                                 "(raise the cap or shrink the corpus)\n",
                                 after_mb, config_.server.max_rss_mb);
                    request_self_stop("rss cap exceeded");
                    return;
                }
                std::fprintf(stderr,
                             "lci-server: RSS %ld -> %ld MB after trim "
                             "(cap %d MB)\n",
                             rss_mb, after_mb, config_.server.max_rss_mb);
            }
        }

        // Idle exit is suppressed while indexing: a big initial index (or
        // /reindex) is work, not idleness.
        if (idle_timeout > std::chrono::seconds(0) &&
            !indexing_active_.load(std::memory_order_acquire)) {
            const auto idle_ns =
                steady_now_ns() -
                last_activity_ns_.load(std::memory_order_acquire);
            if (std::chrono::nanoseconds(idle_ns) >= idle_timeout) {
                request_self_stop("idle timeout reached");
                return;
            }
        }
    }
}

// -- Handler registration -----------------------------------------------------

void IndexServer::register_handlers() {
    // /ping is excluded from activity stamping so liveness probes (client
    // discovery, peer eviction scans) don't keep an otherwise-unused server
    // alive forever.
    svr_.set_pre_routing_handler(
        [this](const httplib::Request& req, httplib::Response&) {
            if (req.path != "/ping") {
                touch_activity();
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

    svr_.Post("/ping",
              [this](const httplib::Request& r, httplib::Response& s) {
                  handle_ping(r, s);
              });
    svr_.Post("/status",
              [this](const httplib::Request& r, httplib::Response& s) {
                  handle_status(r, s);
              });
    svr_.Post("/search",
              [this](const httplib::Request& r, httplib::Response& s) {
                  handle_search(r, s);
              });
    svr_.Post("/symbol",
              [this](const httplib::Request& r, httplib::Response& s) {
                  handle_symbol(r, s);
              });
    svr_.Post("/fileinfo",
              [this](const httplib::Request& r, httplib::Response& s) {
                  handle_fileinfo(r, s);
              });
    svr_.Post("/shutdown",
              [this](const httplib::Request& r, httplib::Response& s) {
                  handle_shutdown(r, s);
              });
    svr_.Post("/reindex",
              [this](const httplib::Request& r, httplib::Response& s) {
                  handle_reindex(r, s);
              });
    svr_.Post("/stats",
              [this](const httplib::Request& r, httplib::Response& s) {
                  handle_stats(r, s);
              });
    svr_.Post("/definition",
              [this](const httplib::Request& r, httplib::Response& s) {
                  handle_definition(r, s);
              });
    svr_.Post("/references",
              [this](const httplib::Request& r, httplib::Response& s) {
                  handle_references(r, s);
              });
    svr_.Post("/tree",
              [this](const httplib::Request& r, httplib::Response& s) {
                  handle_tree(r, s);
              });
    svr_.Post("/git-analyze",
              [this](const httplib::Request& r, httplib::Response& s) {
                  handle_git_analyze(r, s);
              });
    svr_.Post("/list-symbols",
              [this](const httplib::Request& r, httplib::Response& s) {
                  handle_list_symbols(r, s);
              });
    svr_.Post("/inspect-symbol",
              [this](const httplib::Request& r, httplib::Response& s) {
                  handle_inspect_symbol(r, s);
              });
    svr_.Post("/browse-file",
              [this](const httplib::Request& r, httplib::Response& s) {
                  handle_browse_file(r, s);
              });

    // Generic MCP bridge endpoint: `lci mcp` forwards each stdio JSON-RPC
    // frame here so every stdio client shares this server's warmed index
    // instead of re-indexing per process. The dispatcher blocks until the
    // MCP runtime is ready; 501 tells a bridge this server predates (or
    // never enabled) MCP hosting.
    svr_.Post("/mcp",
              [this](const httplib::Request& r, httplib::Response& s) {
                  if (!mcp_dispatcher_) {
                      s.status = 501;
                      s.set_content("mcp hosting not enabled on this server",
                                    "text/plain");
                      return;
                  }
                  std::string out = mcp_dispatcher_(r.body);
                  if (out.empty()) {
                      s.status = 204;  // notification: no response frame
                      return;
                  }
                  s.set_content(out, "application/json");
              });

    // Also register GET handlers for endpoints that don't require a body
    svr_.Get("/ping",
             [this](const httplib::Request& r, httplib::Response& s) {
                 handle_ping(r, s);
             });
    svr_.Get("/status",
             [this](const httplib::Request& r, httplib::Response& s) {
                 handle_status(r, s);
             });
    svr_.Get("/stats",
             [this](const httplib::Request& r, httplib::Response& s) {
                 handle_stats(r, s);
             });
}

// -- Response helpers ---------------------------------------------------------

void IndexServer::json_response(httplib::Response& res,
                                const nlohmann::json& body) {
    res.set_content(body.dump(), "application/json");
    res.status = 200;
}

void IndexServer::error_response(httplib::Response& res, int status,
                                 const std::string& message) {
    nlohmann::json j;
    j["error"] = message;
    res.set_content(j.dump(), "application/json");
    res.status = status;
}

bool IndexServer::require_ready(httplib::Response& res) {
    std::shared_lock lock(mu_);
    if (search_engine_ == nullptr) {
        error_response(res, 503, "index not ready - still indexing");
        return false;
    }
    return true;
}

std::string IndexServer::language_from_path(const std::string& path) const {
    return language_from_extension(path);
}

}  // namespace lci
