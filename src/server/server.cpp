#include <lci/server/server.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
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
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#else
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

// -- Helper: parse symbol kinds -----------------------------------------------

namespace {

using KindSet = std::vector<SymbolType>;

KindSet parse_symbol_kinds(const std::string& kind_str) {
    if (kind_str.empty() || kind_str == "all") {
        return {};
    }
    KindSet result;
    std::string token;
    auto flush = [&] {
        if (token.empty()) return;
        // Normalize to lowercase
        std::string low;
        low.reserve(token.size());
        for (char c : token) {
            low.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        }
        if (low == "func" || low == "fn" || low == "function") {
            result.push_back(SymbolType::Function);
        } else if (low == "type") {
            result.push_back(SymbolType::Type);
            result.push_back(SymbolType::Struct);
            result.push_back(SymbolType::Interface);
            result.push_back(SymbolType::Class);
            result.push_back(SymbolType::Enum);
            result.push_back(SymbolType::Record);
            result.push_back(SymbolType::Trait);
        } else if (low == "struct") {
            result.push_back(SymbolType::Struct);
        } else if (low == "interface" || low == "iface") {
            result.push_back(SymbolType::Interface);
        } else if (low == "method") {
            result.push_back(SymbolType::Method);
        } else if (low == "class" || low == "cls") {
            result.push_back(SymbolType::Class);
        } else if (low == "enum") {
            result.push_back(SymbolType::Enum);
        } else if (low == "variable" || low == "var") {
            result.push_back(SymbolType::Variable);
        } else if (low == "constant" || low == "const") {
            result.push_back(SymbolType::Constant);
        } else if (low == "field") {
            result.push_back(SymbolType::Field);
        }
        token.clear();
    };

    for (char c : kind_str) {
        if (c == ',') {
            flush();
        } else if (c != ' ') {
            token.push_back(c);
        }
    }
    flush();
    return result;
}

bool kind_matches(SymbolType st, const KindSet& kinds) {
    if (kinds.empty()) return true;
    return std::find(kinds.begin(), kinds.end(), st) != kinds.end();
}

// Classifies programming language from file extension via the central
// ext->language table (lci::language_map). Unknown/extensionless paths report
// "" (empty), matching Go's httpLanguageFromPath default and the field-omission
// contract downstream.
std::string language_from_extension(const std::string& path) {
    auto info = language_info_for_path(path);
    if (info.language == LangId::Unknown) return "";
    return std::string(to_string(info.language));
}

double get_rss_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
    return 0.0;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
    }
    return 0.0;
#else
    std::ifstream statm("/proc/self/statm");
    if (statm.is_open()) {
        long pages = 0;
        statm >> pages;  // first field is total size, second is RSS
        statm >> pages;
        long page_size = sysconf(_SC_PAGESIZE);
        return static_cast<double>(pages * page_size) / (1024.0 * 1024.0);
    }
    return 0.0;
#endif
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

    // A live listener on this address means another server already owns this
    // root. Unlinking/rebinding would silently orphan it (its socket file
    // vanishes while the process keeps serving an unreachable inode), so
    // refuse instead of stealing the address.
    {
        Client probe(sock);
        probe.set_timeout(std::chrono::milliseconds{500});
        if (probe.is_server_running()) {
            std::fprintf(stderr,
                         "Error: another index server is already serving %s\n",
                         sock.c_str());
            running_.store(false, std::memory_order_release);
            return false;
        }
    }

#ifndef _WIN32
    // Remove stale socket file (Unix domain socket only)
    std::error_code ec;
    std::filesystem::remove(sock, ec);
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

// -- Endpoint: /ping ----------------------------------------------------------

void IndexServer::handle_ping(const httplib::Request& /*req*/,
                               httplib::Response& res) {
    auto elapsed = std::chrono::steady_clock::now() - start_time_;
    double uptime = std::chrono::duration<double>(elapsed).count();

    std::string bid = build_id_override_.empty() ? build_id()
                                                 : build_id_override_;

    nlohmann::json j;
    j["uptime_seconds"] = uptime;
    j["version"] = kVersion;
    j["build_id"] = bid;
    json_response(res, j);
}

// -- Endpoint: /status --------------------------------------------------------

void IndexServer::handle_status(const httplib::Request& /*req*/,
                                 httplib::Response& res) {
    bool active = indexing_active_.load(std::memory_order_acquire);
    bool ready = false;
    {
        std::shared_lock lock(mu_);
        ready = search_engine_ != nullptr;
    }

    int fc = 0;
    int sc = 0;
    if (ready) {
        auto stats = indexer_->get_stats();
        fc = stats.total_files;
        sc = stats.total_symbols;
    }

    // Live indexing progress. Reads through MasterIndex::get_progress
    // which atomically forwards to the active pipeline's
    // ProgressTracker (lock-free hot path) when a run is in flight, or
    // returns an idle/zero snapshot otherwise. Polling /status during
    // a long index reports increasing files_scanned without racing the
    // pipeline writer.
    auto progress = indexer_->get_progress();
    auto phase_to_string =
        [](MasterIndex::IndexingPhase phase) -> const char* {
            switch (phase) {
                case MasterIndex::IndexingPhase::Scanning: return "scanning";
                case MasterIndex::IndexingPhase::Indexing: return "indexing";
                case MasterIndex::IndexingPhase::Merging:  return "merging";
                case MasterIndex::IndexingPhase::Idle:     return "idle";
            }
            return "idle";
        };

    nlohmann::json indexing_progress;
    indexing_progress["phase"] = phase_to_string(progress.phase);
    indexing_progress["files_scanned"] = progress.files_scanned;
    indexing_progress["files_total"] = progress.files_total;
    indexing_progress["percent_complete"] = progress.percent_complete;
    indexing_progress["elapsed_ms"] = progress.elapsed_ms;

    nlohmann::json j;
    j["ready"] = ready;
    j["file_count"] = fc;
    j["symbol_count"] = sc;
    j["indexing_active"] = active;
    j["progress"] = ready ? 1.0 : 0.0;
    j["indexing_progress"] = std::move(indexing_progress);
    json_response(res, j);
}

// -- Endpoint: /symbol --------------------------------------------------------

void IndexServer::handle_symbol(const httplib::Request& req,
                                 httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    uint64_t symbol_id = body.value("symbol_id", uint64_t{0});
    if (symbol_id == 0) {
        error_response(res, 400, "symbol_id is required");
        return;
    }

    auto rt_snap = indexer_->ref_tracker().pin();
    auto sym = rt_snap->get_enhanced_symbol(symbol_id);
    if (sym == nullptr) {
        error_response(res, 404, "symbol not found");
        return;
    }

    nlohmann::json j;
    j["symbol"]["name"] = sym->symbol.name;
    j["symbol"]["type"] = std::string(to_string(sym->symbol.type));
    j["symbol"]["file_id"] = sym->symbol.file_id;
    j["symbol"]["line"] = sym->symbol.line;
    j["symbol"]["signature"] = sym->signature;
    j["symbol"]["is_exported"] = sym->is_exported;
    j["symbol"]["doc_comment"] = sym->doc_comment;
    json_response(res, j);
}

// -- Endpoint: /fileinfo ------------------------------------------------------

void IndexServer::handle_fileinfo(const httplib::Request& req,
                                   httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    auto file_id = static_cast<FileID>(body.value("file_id", 0u));
    if (file_id == 0) {
        error_response(res, 400, "file_id is required");
        return;
    }

    auto path = indexer_->get_file_path(file_id);
    if (path.empty()) {
        error_response(res, 404, "file not found");
        return;
    }

    auto rt_snap = indexer_->ref_tracker().pin();
    auto symbols = rt_snap->get_file_enhanced_symbols(file_id);

    nlohmann::json j;
    j["file_info"]["file_id"] = file_id;
    j["file_info"]["path"] = path;
    j["file_info"]["symbol_count"] = static_cast<int>(symbols.size());
    json_response(res, j);
}

// -- Endpoint: /shutdown ------------------------------------------------------

void IndexServer::handle_shutdown(const httplib::Request& req,
                                   httplib::Response& res) {
    bool force = false;
    try {
        auto body = nlohmann::json::parse(req.body);
        force = body.value("force", false);
    } catch (const nlohmann::json::exception&) {
        // Empty/absent body means a plain graceful shutdown.
    }

    nlohmann::json j;
    j["success"] = true;
    j["message"] = "Server shutting down";
    json_response(res, j);

    // Trigger shutdown after the response flushes. Owned (not detached) and
    // joined in shutdown()/dtor, so it can never outlive this server and touch
    // freed members. CAS guards a single spawn across concurrent /shutdown
    // requests (a second request must not overwrite a joinable thread).
    bool expected = false;
    if (shutdown_triggered_.compare_exchange_strong(expected, true)) {
        shutdown_trigger_ = std::thread([this, force] {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            // Clear running_ too: the CLI serve loop exits on
            // !is_running(), and before this /shutdown only flipped
            // shutdown_requested_ (observed by wait(), which the CLI
            // does not call) — a remote /shutdown left the process
            // serving forever. Peer eviction depends on this working.
            running_.store(false, std::memory_order_release);
            // Stop accepting now, whether or not any owner polls
            // is_running() — an embedded server with no watching owner
            // must not keep serving after promising to shut down.
            svr_.stop();
            {
                std::lock_guard lock(shutdown_mu_);
                shutdown_requested_ = true;
            }
            shutdown_cv_.notify_all();
            if (force) {
                // force means "guarantee this process dies": arm a watchdog
                // that exits hard after a grace window. A healthy server
                // tears down and exits normally well within it; only a hung
                // teardown reaches the _Exit. Detaching is safe here — the
                // watchdog captures nothing and touches no members, so it
                // cannot use-after-free (unlike the trigger thread itself,
                // which is owned and joined for exactly that reason).
                std::thread([] {
                    std::this_thread::sleep_for(std::chrono::seconds{5});
                    std::_Exit(0);
                }).detach();
            }
        });
    }
}

// -- Endpoint: /reindex -------------------------------------------------------

void IndexServer::handle_reindex(const httplib::Request& req,
                                  httplib::Response& res) {
    // A server that has decided to stop (self-stop, /shutdown in flight)
    // must refuse: the swap below would cancel-and-join the new run
    // immediately AFTER the lambda already cleared the engine and the
    // index, leaving a permanently bricked server (engine null,
    // indexing_active_ stuck true, every endpoint 503).
    if (!running_.load(std::memory_order_acquire)) {
        error_response(res, 503, "server is shutting down");
        return;
    }

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        body = nlohmann::json::object();
    }

    std::string root_path = body.value("path", "");
    if (root_path.empty()) {
        root_path = config_.project.root;
    }

    // Atomically cancel any prior in-flight indexing run and install
    // the fresh one. Without atomic cancel-and-replace, two concurrent
    // reindex requests would race: both could observe an empty slot
    // and try to install their own thread, terminating the process on
    // the second assignment to a joinable thread. swap_indexing_thread
    // serialises this on `indexing_thread_mu_`.
    //
    // `indexing_active_` is set to true *before* the swap so observers
    // never see a transient false between "cancelled prior run" and
    // "started new run". The lambda only clears it when the run reaches
    // completion (either by publishing a new search engine or by
    // observing shutdown); a cancelled run leaves the flag set so the
    // successor thread covers it.
    indexing_active_.store(true, std::memory_order_release);
    swap_indexing_thread(std::thread([this, root_path] {
        {
            std::unique_lock engine_lock(mu_);
            search_engine_ = nullptr;
            owned_search_engine_.reset();
        }

        indexer_->clear();
        indexer_->index_directory(root_path);

        // Bail out (without clearing indexing_active_) if a successor
        // reindex superseded us — the successor is responsible for
        // clearing the flag once it publishes its own engine. If the
        // server is shutting down, clear the flag so /status is
        // accurate during the brief window before the server exits.
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

    nlohmann::json j;
    j["success"] = true;
    j["message"] = "Re-indexing started for " + root_path;
    json_response(res, j);
}

// -- Endpoint: /stats ---------------------------------------------------------

void IndexServer::handle_stats(const httplib::Request& /*req*/,
                                httplib::Response& res) {
    if (!require_ready(res)) return;

    auto stats = indexer_->get_stats();
    auto elapsed = std::chrono::steady_clock::now() - start_time_;
    double uptime = std::chrono::duration<double>(elapsed).count();

    nlohmann::json j;
    j["file_count"] = stats.total_files;
    j["symbol_count"] = stats.total_symbols;
    j["index_size_bytes"] =
        static_cast<int64_t>(indexer_->index_size_bytes());
    j["build_duration_ms"] = stats.indexing_time_ns / 1'000'000;
    j["memory_rss_mb"] = get_rss_mb();
    j["num_threads"] = static_cast<int>(std::thread::hardware_concurrency());
    j["uptime_seconds"] = uptime;
    json_response(res, j);
}

// -- Endpoint: /tree ----------------------------------------------------------

void IndexServer::handle_tree(const httplib::Request& req,
                               httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    auto function_name = body.value("function_name", "");
    if (function_name.empty()) {
        error_response(res, 400, "function_name is required");
        return;
    }

    int max_depth = body.value("max_depth", 0);

    // Find the symbol by name
    auto rt_snap = indexer_->ref_tracker().pin();
    auto symbols = rt_snap->find_symbols_by_name(function_name);
    if (symbols.empty()) {
        nlohmann::json j;
        j["error"] = "function not found: " + function_name;
        json_response(res, j);
        return;
    }

    const auto& sym = symbols[0];
    auto tree = indexer_->ref_tracker().build_function_tree(
        sym->id, max_depth > 0 ? max_depth : 10);

    // Serialize tree recursively. Mirrors Go's tree node shape, which
    // includes annotations / safety / impact fields (left null/zero in
    // the C++ port until the analyzers that produce them are wired in).
    std::function<int(const FunctionTreeNode&)> count_nodes;
    count_nodes = [&](const FunctionTreeNode& node) -> int {
        int n = 1;
        for (const auto& child : node.children) {
            n += count_nodes(child);
        }
        return n;
    };

    // Normalize file_path to relative-to-project-root so tree paths
    // match what /search and /git-analyze emit.
    auto rel_tree_path = [&](const std::string& p) -> std::string {
        if (p.empty()) return p;
        std::filesystem::path abs_path(p);
        if (!abs_path.is_absolute()) return p;
        std::error_code ec;
        auto rel = std::filesystem::relative(abs_path,
                                             config_.project.root, ec);
        if (ec || rel.empty()) return p;
        return rel.generic_string();
    };

    std::function<nlohmann::json(const FunctionTreeNode&, int)> serialize_node;
    serialize_node = [&](const FunctionTreeNode& node, int depth) -> nlohmann::json {
        nlohmann::json nj;
        nj["name"] = node.name;
        nj["line"] = node.line;
        nj["depth"] = depth;
        // Resolve file_path from the node's file_id so CLI consumers can
        // render `[path:line]` annotations and look up per-symbol metrics
        // via /browse-file. Empty string for unresolved nodes (root with
        // no symbol bound, recursion guards, etc.).
        nj["file_path"] =
            node.file_id != 0
                ? rel_tree_path(indexer_->get_file_path(node.file_id))
                : "";
        nj["node_type"] = 0;
        nj["dependency_count"] = static_cast<int>(node.children.size());
        nj["dependent_count"] = 0;
        nj["edit_risk_score"] = 0;
        nj["impact_radius"] = 0;
        nj["annotations"] = nullptr;
        nj["safety_notes"] = nullptr;
        nj["stability_tags"] = nullptr;

        nlohmann::json children = nlohmann::json::array();
        for (const auto& child : node.children) {
            children.push_back(serialize_node(child, depth + 1));
        }
        nj["children"] = children;
        return nj;
    };

    int total_nodes = count_nodes(tree) - 1;  // exclude root from count

    nlohmann::json options;
    options["agent_mode"] = false;
    options["compact"] = false;
    options["exclude_pattern"] = "";
    options["max_depth"] = max_depth;
    options["show_lines"] = false;

    nlohmann::json tree_j;
    tree_j["root"] = serialize_node(tree, 0);
    tree_j["root_function"] = function_name;
    tree_j["max_depth"] = max_depth;
    tree_j["options"] = options;
    tree_j["total_nodes"] = total_nodes;

    nlohmann::json j;
    j["tree"] = tree_j;
    json_response(res, j);
}

// -- Endpoint: /git-analyze ---------------------------------------------------

void IndexServer::handle_git_analyze(const httplib::Request& req,
                                       httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    auto scope = body.value("scope", "");
    if (scope.empty()) {
        error_response(res, 400, "scope is required");
        return;
    }

    git::Provider provider;
    if (!git::Provider::create(config_.project.root, provider)) {
        // Absent precondition, not a request error: 200 with an explicit
        // not-applicable payload, matching the MCP git surfaces.
        nlohmann::json na;
        na["available"] = false;
        na["reason"] = "not a git repository";
        json_response(res, na);
        return;
    }

    git::AnalysisParams params = git::AnalysisParams::defaults();
    if (scope == "staged") {
        params.scope = git::AnalysisScope::Staged;
    } else if (scope == "wip") {
        params.scope = git::AnalysisScope::WIP;
    } else if (scope == "commit") {
        params.scope = git::AnalysisScope::Commit;
    } else if (scope == "range") {
        params.scope = git::AnalysisScope::Range;
    } else {
        error_response(res, 400, "invalid scope");
        return;
    }

    params.base_ref = body.value("base_ref", "");
    params.target_ref = body.value("target_ref", "");
    params.similarity_threshold = body.value("similarity_threshold", 0.8);
    params.max_findings = body.value("max_findings", 20);
    if (body.contains("focus") && body["focus"].is_array()) {
        params.focus = body["focus"].get<std::vector<std::string>>();
    }

    git::Analyzer analyzer(provider, *indexer_);
    git::AnalysisReport report;
    if (!analyzer.analyze(params, report)) {
        error_response(res, 500, "git analyze failed");
        return;
    }

    nlohmann::json j;
    j["report"] = git::report_to_json(report, config_.project.root);
    json_response(res, j);
}

// -- Endpoint: /list-symbols --------------------------------------------------

void IndexServer::handle_list_symbols(const httplib::Request& req,
                                       httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    auto kind_str = body.value("kind", "");
    auto kinds = parse_symbol_kinds(kind_str);
    auto file_filter = body.value("file", "");
    auto name_filter = body.value("name", "");
    auto receiver_filter = body.value("receiver", "");
    std::optional<bool> exported_filter;
    if (body.contains("exported") && !body["exported"].is_null()) {
        exported_filter = body["exported"].get<bool>();
    }
    std::optional<int> min_complexity, max_complexity;
    std::optional<int> min_params, max_params;
    if (body.contains("min_complexity") && !body["min_complexity"].is_null()) {
        min_complexity = body["min_complexity"].get<int>();
    }
    if (body.contains("max_complexity") && !body["max_complexity"].is_null()) {
        max_complexity = body["max_complexity"].get<int>();
    }
    if (body.contains("min_params") && !body["min_params"].is_null()) {
        min_params = body["min_params"].get<int>();
    }
    if (body.contains("max_params") && !body["max_params"].is_null()) {
        max_params = body["max_params"].get<int>();
    }
    const auto page = normalize_page(body);
    const int max_results = page.max;
    const int offset = page.offset;
    const auto sort_key = body.value("sort", "");

    auto all_file_ids = indexer_->get_all_file_ids();
    // Sort by file_id ascending for deterministic output ordering.
    // get_all_file_ids() iterates a hash map and returns ids in arbitrary
    // order; without sorting, list-symbols output is non-reproducible across
    // runs and diverges from the Go reference (which iterates files in
    // insertion / file_id order).
    std::sort(all_file_ids.begin(), all_file_ids.end());

    // Collect ALL matching symbols before paging. The sort (when requested)
    // must run over the full filtered set: sorting a pre-capped page turns
    // "top N by complexity" into "an arbitrary page, reordered" — the same
    // cap-before-score defect class fixed in find_files (1d00e11).
    struct Row {
        ReferenceTracker::Snapshot::SymbolHandle sym;
        uint32_t path_idx;
    };
    std::vector<Row> rows;
    std::vector<std::string> row_paths;  // One entry per contributing file.

    auto rt_snap = indexer_->ref_tracker().pin();
    for (auto fid : all_file_ids) {
        auto file_path = indexer_->get_file_path(fid);
        if (file_path.empty()) continue;

        if (!file_filter.empty()) {
            auto base = std::filesystem::path(file_path).filename().string();
            bool matched = (file_path.find(file_filter) != std::string::npos) ||
                           (base.find(file_filter) != std::string::npos);
            if (!matched) continue;
        }

        auto symbols = rt_snap->get_file_enhanced_symbols(fid);
        for (const auto& sym : symbols) {
            if (!kind_matches(sym->symbol.type, kinds)) continue;
            if (exported_filter.has_value()) {
                if (*exported_filter != sym->is_exported) continue;
            }
            if (!name_filter.empty() &&
                !text::ascii_contains_ci(sym->symbol.name, name_filter)) {
                continue;
            }
            if (!receiver_filter.empty() &&
                text::ascii_lower(sym->receiver_type) != text::ascii_lower(receiver_filter)) {
                continue;
            }
            if (min_complexity.has_value() &&
                sym->complexity < *min_complexity) {
                continue;
            }
            if (max_complexity.has_value() &&
                sym->complexity > *max_complexity) {
                continue;
            }
            if (min_params.has_value() &&
                static_cast<int>(sym->parameter_count) < *min_params) {
                continue;
            }
            if (max_params.has_value() &&
                static_cast<int>(sym->parameter_count) > *max_params) {
                continue;
            }

            if (rows.empty() || row_paths.back() != file_path) {
                row_paths.push_back(file_path);
            }
            rows.push_back(
                {sym, static_cast<uint32_t>(row_paths.size() - 1)});
        }
    }

    // Server-side sort mirrors the CLI's sym_sort_symbols keys exactly
    // (complexity/refs/params descending, line/name ascending, empty =
    // deterministic file/line collection order). stable_sort over the
    // deterministic collection order keeps ties reproducible across runs.
    if (!sort_key.empty()) {
        auto by = [&](auto key_less) {
            std::stable_sort(rows.begin(), rows.end(), key_less);
        };
        if (sort_key == "complexity") {
            by([](const Row& a, const Row& b) {
                return a.sym->complexity > b.sym->complexity;
            });
        } else if (sort_key == "refs") {
            by([](const Row& a, const Row& b) {
                return a.sym->incoming_ref_count + a.sym->outgoing_ref_count >
                       b.sym->incoming_ref_count + b.sym->outgoing_ref_count;
            });
        } else if (sort_key == "params") {
            by([](const Row& a, const Row& b) {
                return a.sym->parameter_count > b.sym->parameter_count;
            });
        } else if (sort_key == "line") {
            by([&](const Row& a, const Row& b) {
                if (a.path_idx != b.path_idx) {
                    return row_paths[a.path_idx] < row_paths[b.path_idx];
                }
                return a.sym->symbol.line < b.sym->symbol.line;
            });
        } else {
            // Default + unknown -> name (ascending), like the CLI.
            by([](const Row& a, const Row& b) {
                return a.sym->symbol.name < b.sym->symbol.name;
            });
        }
    }

    const int total = static_cast<int>(rows.size());
    nlohmann::json entries = nlohmann::json::array();
    for (int i = offset;
         i < total && static_cast<int>(entries.size()) < max_results; ++i) {
        const auto& sym = rows[static_cast<size_t>(i)].sym;
        const std::string& file_path =
            row_paths[rows[static_cast<size_t>(i)].path_idx];

        // Mirror Go's `json:",omitempty"` semantics: only emit
        // numeric/string fields when non-zero / non-empty so that
        // canonicalised JSON matches the Go reference output.
        // Note: Go's /list-symbols handler intentionally omits the
        // `signature` field (it's exposed only via /inspect-symbol
        // in the Go reference). Match that here so summary listings
        // stay identical and signatures only appear where Go
        // surfaces them.
        nlohmann::json e;
        e["name"] = sym->symbol.name;
        e["type"] = std::string(to_string(sym->symbol.type));
        e["file"] = file_path;
        e["line"] = sym->symbol.line;
        e["object_id"] = encode_symbol_id(sym->id);
        e["is_exported"] = sym->is_exported;
        if (sym->complexity > 0) e["complexity"] = sym->complexity;
        if (sym->parameter_count > 0) {
            e["parameter_count"] = static_cast<int>(sym->parameter_count);
        }
        if (!sym->receiver_type.empty()) {
            e["receiver_type"] = sym->receiver_type;
        }
        if (sym->incoming_ref_count != 0) {
            e["incoming_refs"] = static_cast<int>(sym->incoming_ref_count);
        }
        if (sym->outgoing_ref_count != 0) {
            e["outgoing_refs"] = static_cast<int>(sym->outgoing_ref_count);
        }
        entries.push_back(e);
    }

    nlohmann::json j;
    j["symbols"] = entries;
    j["total"] = total;
    j["showing"] = static_cast<int>(entries.size());
    j["has_more"] =
        page_has_more(total, offset, static_cast<int>(entries.size()));
    json_response(res, j);
}

// -- Endpoint: /inspect-symbol ------------------------------------------------

void IndexServer::handle_inspect_symbol(const httplib::Request& req,
                                         httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    std::vector<ReferenceTracker::Snapshot::SymbolHandle> matched;

    // Pin the RCU snapshot for the lifetime of every pointer pulled from the
    // tracker below (matched[] elements and the type-hierarchy lookups all
    // point into this snapshot; it must outlive the serialization loop).
    auto rt_snap = indexer_->ref_tracker().pin();

    // Try by ID first
    auto id_str = body.value("id", "");
    if (!id_str.empty()) {
        auto decoded = decode_symbol_id(id_str);
        if (decoded.has_value()) {
            auto sym =
                rt_snap->get_enhanced_symbol(decoded.value());
            if (sym != nullptr) {
                matched.push_back(sym);
            }
        }
    }

    // Try by name if no ID match
    auto name_str = body.value("name", "");
    if (matched.empty() && !name_str.empty()) {
        matched = rt_snap->find_symbols_by_name(name_str);
    }

    // Apply disambiguators (file, type)
    auto file_filter = body.value("file", "");
    auto type_filter = body.value("type", "");
    if (!file_filter.empty() || !type_filter.empty()) {
        auto type_kinds = parse_symbol_kinds(type_filter);

        std::vector<ReferenceTracker::Snapshot::SymbolHandle> filtered;
        for (const auto& sym : matched) {
            if (!file_filter.empty()) {
                auto fp = indexer_->get_file_path(sym->symbol.file_id);
                auto base = std::filesystem::path(fp).filename().string();
                if (fp.find(file_filter) == std::string::npos &&
                    base.find(file_filter) == std::string::npos) {
                    continue;
                }
            }
            if (!type_kinds.empty() &&
                !kind_matches(sym->symbol.type, type_kinds)) {
                continue;
            }
            filtered.push_back(sym);
        }
        matched = filtered;
    }

    auto include_raw = body.value("include", "");
    const bool include_signature =
        include_raw == "all" || include_raw == "signature" ||
        include_raw.find("signature") != std::string::npos;

    auto& tracker = indexer_->ref_tracker();

    nlohmann::json symbols = nlohmann::json::array();
    for (const auto& sym : matched) {
        auto fp = indexer_->get_file_path(sym->symbol.file_id);

        nlohmann::json e;
        e["name"] = sym->symbol.name;
        e["object_id"] = encode_symbol_id(sym->id);
        e["type"] = std::string(to_string(sym->symbol.type));
        e["file"] = fp;
        e["line"] = sym->symbol.line;
        // Symbol bounds: emit `end_line` (and derived `lines_of_code`) when the
        // extractor populated it. Consumed by the CLI's `--enhanced` /
        // `--assembly` modes to render the surrounding block. Omitted with
        // `>` parity to git-analyze (which uses the same gating).
        if (sym->symbol.end_line > sym->symbol.line) {
            e["end_line"] = sym->symbol.end_line;
            e["lines_of_code"] =
                sym->symbol.end_line - sym->symbol.line + 1;
        }
        e["is_exported"] = sym->is_exported;
        e["complexity"] = sym->complexity;
        e["outgoing_refs"] = static_cast<int>(sym->outgoing_ref_count);
        if (include_signature && !sym->signature.empty()) {
            e["signature"] = sym->signature;
        }
        if (!sym->doc_comment.empty()) {
            e["doc_comment"] = sym->doc_comment;
        }
        if (sym->parameter_count > 0) {
            e["parameter_count"] = static_cast<int>(sym->parameter_count);
        }
        if (!sym->receiver_type.empty()) {
            e["receiver_type"] = sym->receiver_type;
        }
        if (sym->incoming_ref_count != 0) {
            e["incoming_refs"] = static_cast<int>(sym->incoming_ref_count);
        }

        // Callers/callees
        auto callers = tracker.get_caller_names(sym->id);
        if (!callers.empty()) {
            e["callers"] = callers;
        }
        auto callees = tracker.get_callee_names(sym->id);
        if (!callees.empty()) {
            e["callees"] = callees;
        }

        // Type hierarchy
        auto rels = tracker.get_type_relationships(sym->id);
        if (rels.has_relationships()) {
            nlohmann::json th;
            th["implements"] = nlohmann::json::array();
            th["implemented_by"] = nlohmann::json::array();
            th["extends"] = nlohmann::json::array();
            th["extended_by"] = nlohmann::json::array();

            for (auto id : rels.implements) {
                if (auto s = rt_snap->get_enhanced_symbol(id)) {
                    th["implements"].push_back(s->symbol.name);
                }
            }
            for (auto id : rels.implemented_by) {
                if (auto s = rt_snap->get_enhanced_symbol(id)) {
                    th["implemented_by"].push_back(s->symbol.name);
                }
            }
            for (auto id : rels.extends) {
                if (auto s = rt_snap->get_enhanced_symbol(id)) {
                    th["extends"].push_back(s->symbol.name);
                }
            }
            for (auto id : rels.extended_by) {
                if (auto s = rt_snap->get_enhanced_symbol(id)) {
                    th["extended_by"].push_back(s->symbol.name);
                }
            }
            e["type_hierarchy"] = th;
        }

        // Scope chain
        if (!sym->scope_chain.empty()) {
            nlohmann::json chain = nlohmann::json::array();
            for (const auto& sc : sym->scope_chain) {
                chain.push_back(sc.name);
            }
            e["scope_chain"] = chain;
        }

        // Function flags
        if (sym->function_flags != 0) {
            nlohmann::json flags = nlohmann::json::array();
            if (sym->is_async_func()) flags.push_back("async");
            if (sym->is_generator_func()) flags.push_back("generator");
            if (sym->is_method_func()) flags.push_back("method");
            if (sym->is_variadic_func()) flags.push_back("variadic");
            e["function_flags"] = flags;
        }

        // Variable flags
        if (sym->variable_flags != 0) {
            nlohmann::json flags = nlohmann::json::array();
            if (sym->is_const()) flags.push_back("const");
            if (sym->is_static()) flags.push_back("static");
            if (sym->is_pointer()) flags.push_back("pointer");
            e["variable_flags"] = flags;
        }

        symbols.push_back(e);
    }

    nlohmann::json j;
    j["symbols"] = symbols;
    j["count"] = static_cast<int>(symbols.size());
    json_response(res, j);
}

// -- Endpoint: /browse-file ---------------------------------------------------

void IndexServer::handle_browse_file(const httplib::Request& req,
                                      httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    FileID target_fid = 0;
    std::string target_path;
    bool found = false;

    // Try by file_id first
    if (body.contains("file_id") && !body["file_id"].is_null()) {
        target_fid = static_cast<FileID>(body["file_id"].get<int>());
        target_path = indexer_->get_file_path(target_fid);
        if (!target_path.empty()) found = true;
    }

    // Try by file path
    auto file_str = body.value("file", "");
    if (!found && !file_str.empty()) {
        auto all_ids = indexer_->get_all_file_ids();
        for (auto fid : all_ids) {
            auto fp = indexer_->get_file_path(fid);
            if (fp.empty()) continue;

            bool match = (fp == file_str);
            if (!match) {
                // Check suffix match
                if (fp.size() > file_str.size()) {
                    auto sep = fp[fp.size() - file_str.size() - 1];
                    match = (sep == '/' || sep == '\\') &&
                            fp.substr(fp.size() - file_str.size()) == file_str;
                }
            }
            if (match) {
                target_fid = fid;
                target_path = fp;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        nlohmann::json j;
        j["error"] = "file not found: " + file_str;
        json_response(res, j);
        return;
    }

    auto kind_str = body.value("kind", "");
    auto kinds = parse_symbol_kinds(kind_str);
    std::optional<bool> exported_filter;
    if (body.contains("exported") && !body["exported"].is_null()) {
        exported_filter = body["exported"].get<bool>();
    }
    int max_results = body.value("max", 100);
    if (max_results <= 0) max_results = 100;

    auto rt_snap = indexer_->ref_tracker().pin();
    auto symbols = rt_snap->get_file_enhanced_symbols(target_fid);

    nlohmann::json entries = nlohmann::json::array();
    int total = 0;
    for (const auto& sym : symbols) {
        if (!kind_matches(sym->symbol.type, kinds)) continue;
        if (exported_filter.has_value() &&
            *exported_filter != sym->is_exported) {
            continue;
        }
        ++total;
        if (static_cast<int>(entries.size()) >= max_results) continue;

        // Same omitempty treatment as /list-symbols so HTTP browse-file
        // shape matches Go's reference encoder field-for-field. Go's
        // /browse-file (like /list-symbols) intentionally omits the
        // `signature` field, surfacing it only through /inspect-symbol.
        nlohmann::json e;
        e["name"] = sym->symbol.name;
        e["type"] = std::string(to_string(sym->symbol.type));
        e["file"] = target_path;
        e["line"] = sym->symbol.line;
        // Same end_line/lines_of_code emission as /list-symbols so the CLI's
        // enhanced/assembly output modes can resolve enclosing-block bounds
        // via either entry point. Gated on `end_line > line` to avoid
        // poisoning consumers with unset zero-bounds rows.
        if (sym->symbol.end_line > sym->symbol.line) {
            e["end_line"] = sym->symbol.end_line;
            e["lines_of_code"] =
                sym->symbol.end_line - sym->symbol.line + 1;
        }
        e["object_id"] = encode_symbol_id(sym->id);
        e["is_exported"] = sym->is_exported;
        if (sym->complexity > 0) e["complexity"] = sym->complexity;
        if (sym->parameter_count > 0) {
            e["parameter_count"] = static_cast<int>(sym->parameter_count);
        }
        if (!sym->receiver_type.empty()) {
            e["receiver_type"] = sym->receiver_type;
        }
        if (sym->incoming_ref_count != 0) {
            e["incoming_refs"] = static_cast<int>(sym->incoming_ref_count);
        }
        if (sym->outgoing_ref_count != 0) {
            e["outgoing_refs"] = static_cast<int>(sym->outgoing_ref_count);
        }
        entries.push_back(e);
    }

    nlohmann::json j;
    j["file"]["path"] = target_path;
    j["file"]["file_id"] = static_cast<int>(target_fid);
    j["file"]["language"] = language_from_path(target_path);
    j["symbols"] = entries;
    j["total"] = total;

    // Optional imports
    if (body.value("show_imports", false)) {
        auto fc = indexer_->file_content_store().get_file(target_fid);
        // Imports are stored on FileInfo which isn't directly accessible
        // from the current C++ API. Return empty for now.
        j["imports"] = nlohmann::json::array();
    }

    // Optional stats
    if (body.value("show_stats", false)) {
        int func_count = 0;
        int type_count = 0;
        int exported_count = 0;
        int max_cx = 0;
        int total_cx = 0;
        int cx_count = 0;

        for (const auto& sym : symbols) {
            if (sym->is_exported) ++exported_count;
            if (sym->symbol.type == SymbolType::Function ||
                sym->symbol.type == SymbolType::Method) {
                ++func_count;
                if (sym->complexity > 0) {
                    total_cx += sym->complexity;
                    ++cx_count;
                    if (sym->complexity > max_cx) max_cx = sym->complexity;
                }
            } else if (sym->symbol.type == SymbolType::Type ||
                       sym->symbol.type == SymbolType::Struct ||
                       sym->symbol.type == SymbolType::Interface ||
                       sym->symbol.type == SymbolType::Class ||
                       sym->symbol.type == SymbolType::Enum) {
                ++type_count;
            }
        }

        nlohmann::json stats;
        stats["symbol_count"] = static_cast<int>(symbols.size());
        stats["function_count"] = func_count;
        stats["type_count"] = type_count;
        stats["avg_complexity"] =
            cx_count > 0 ? static_cast<double>(total_cx) / cx_count : 0.0;
        stats["max_complexity"] = max_cx;
        stats["exported_count"] = exported_count;
        j["stats"] = stats;
    }

    json_response(res, j);
}

}  // namespace lci
