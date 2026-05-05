#include <lci/server/server.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <thread>

#include <lci/core/reference_tracker.h>
#include <lci/file_info.h>
#include <lci/git/analyzer.h>
#include <lci/git/provider.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/search/search_engine.h>
#include <lci/search/search_options.h>
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
/// On Windows, returns a localhost TCP address "localhost:<port>".
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

}  // namespace

std::string get_socket_path() {
    const uint32_t uid = current_user_id();
#ifdef _WIN32
    // Per-user offset within the 1000-port project window so two users on
    // the same Windows host don't both bind kWindowsBasePort.
    const int port = kWindowsBasePort + static_cast<int>(uid % 1000);
    return "localhost:" + std::to_string(port);
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
    return "localhost:" + std::to_string(port);
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

std::string to_lower(std::string_view sv) {
    std::string r;
    r.reserve(sv.size());
    for (char c : sv) {
        r.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return r;
}

bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    auto h = to_lower(haystack);
    auto n = to_lower(needle);
    return h.find(n) != std::string::npos;
}

std::string language_from_extension(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    auto low = to_lower(ext);
    if (low == ".go") return "go";
    if (low == ".js") return "javascript";
    if (low == ".ts") return "typescript";
    if (low == ".tsx") return "tsx";
    if (low == ".jsx") return "jsx";
    if (low == ".py") return "python";
    if (low == ".rs") return "rust";
    if (low == ".java") return "java";
    if (low == ".cs") return "csharp";
    if (low == ".cpp" || low == ".cc" || low == ".cxx") return "cpp";
    if (low == ".c") return "c";
    if (low == ".rb") return "ruby";
    if (low == ".php") return "php";
    if (low == ".kt") return "kotlin";
    if (low == ".zig") return "zig";
    return "";
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
    if (running_.load(std::memory_order_relaxed)) {
        shutdown(std::chrono::milliseconds{5000});
    }
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

bool IndexServer::is_running() const {
    return running_.load(std::memory_order_acquire);
}

// -- Server lifecycle ---------------------------------------------------------

bool IndexServer::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return false;  // already running
    }

    auto sock = socket_path();

#ifndef _WIN32
    // Remove stale socket file (Unix domain socket only)
    std::error_code ec;
    std::filesystem::remove(sock, ec);
#endif

    register_handlers();
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
    }

#ifdef _WIN32
    // On Windows, use TCP localhost. Parse "localhost:<port>" from sock.
    int win_port = 0;
    auto colon_pos = sock.rfind(':');
    if (colon_pos != std::string::npos) {
        win_port = std::stoi(sock.substr(colon_pos + 1));
    }
    if (win_port <= 0) {
        running_.store(false, std::memory_order_release);
        return false;
    }

    listen_thread_ = std::thread([this, win_port] {
        if (!svr_.bind_to_port("127.0.0.1", win_port)) {
            running_.store(false, std::memory_order_release);
            return;
        }
        svr_.listen_after_bind();
    });

    // Brief wait for the server to be ready
    for (int i = 0; i < 50; ++i) {
        if (svr_.is_running()) break;
        if (!running_.load(std::memory_order_acquire)) return false;
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

        chmod(sock.c_str(), 0600);
        svr_.listen_after_bind();
    });

    // Brief wait for the socket to be ready
    for (int i = 0; i < 50; ++i) {
        if (std::filesystem::exists(sock)) break;
        if (!running_.load(std::memory_order_acquire)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
#endif

    return running_.load(std::memory_order_acquire);
}

void IndexServer::wait() {
    std::unique_lock lock(shutdown_mu_);
    shutdown_cv_.wait(lock, [this] { return shutdown_requested_; });
}

bool IndexServer::shutdown(std::chrono::milliseconds /*timeout*/) {
    bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (!was_running) {
        return true;
    }

    svr_.stop();

    if (listen_thread_.joinable()) {
        listen_thread_.join();
    }

    // Cooperatively cancel any in-flight indexing and join its thread
    // before continuing teardown. Without this, a long-running indexing
    // run could outlive the server (use-after-free on indexer_,
    // owned_indexer_, mu_, etc.).
    cancel_indexing_thread();

#ifndef _WIN32
    // Remove socket file (Unix domain socket only)
    std::error_code ec;
    std::filesystem::remove(socket_path(), ec);
#endif

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

// -- Handler registration -----------------------------------------------------

void IndexServer::register_handlers() {
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

    nlohmann::json j;
    j["ready"] = ready;
    j["file_count"] = fc;
    j["symbol_count"] = sc;
    j["indexing_active"] = active;
    j["progress"] = ready ? 1.0 : 0.0;
    json_response(res, j);
}

// -- Endpoint: /search --------------------------------------------------------

void IndexServer::handle_search(const httplib::Request& req,
                                 httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    auto pattern = body.value("pattern", "");
    if (pattern.empty()) {
        error_response(res, 400, "pattern is required");
        return;
    }

    SearchOptions opts;
    opts.max_results = body.value("max_results", 100);
    opts.case_insensitive = body.value("case_insensitive", false);
    opts.declaration_only = body.value("declaration_only", false);
    // Go's /search ships a `context` block per hit. Request a small
    // surrounding window so extract_context() actually populates lines;
    // without this max_context_lines=0 produces an empty `lines` array.
    opts.max_context_lines = body.value("max_context_lines", 1);

    std::vector<SearchResult> results;
    {
        std::shared_lock lock(mu_);
        results = indexer_->search_with_options(pattern, opts);
    }

    int max_res = body.value("max_results", 0);
    if (max_res > 0 && static_cast<int>(results.size()) > max_res) {
        results.resize(static_cast<size_t>(max_res));
    }

    nlohmann::json j;
    j["results"] = nlohmann::json::array();
    for (const auto& r : results) {
        nlohmann::json rj;
        rj["file_id"] = static_cast<int>(r.file_id);
        rj["path"] = r.path;
        rj["line"] = r.line;
        rj["column"] = r.column;
        rj["match"] = r.match_text;
        rj["score"] = r.score;

        // Build the structured `context` block Go's /search emits. The
        // search engine populates `context.lines` for the surrounding
        // window and the matched line number; the rest of the fields are
        // derived from those plus the SearchContext metadata.
        nlohmann::json ctx;
        ctx["block_type"] = r.context.block_type.empty()
                                ? std::string("lines")
                                : r.context.block_type;
        // `block_name` is the enclosing function/class/struct identifier when
        // the search engine could resolve one for the match. Always emit the
        // key (empty string when no enclosing block) so CLI consumers can rely
        // on the field being present and the JSON shape stays stable across
        // matches with and without semantic context.
        ctx["block_name"] = r.context.block_name;
        ctx["start_line"] = r.context.start_line;
        ctx["end_line"] = r.context.end_line;
        ctx["is_complete"] = r.context.is_complete;

        nlohmann::json lines_arr = nlohmann::json::array();
        for (const auto& l : r.context.lines) {
            lines_arr.push_back(l);
        }
        ctx["lines"] = lines_arr;

        nlohmann::json matched = nlohmann::json::array();
        matched.push_back(r.line);
        ctx["matched_lines"] = matched;
        ctx["match_count"] = 1;

        rj["context"] = ctx;
        j["results"].push_back(rj);
    }
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

    const auto* sym = indexer_->ref_tracker().get_enhanced_symbol(symbol_id);
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

    auto symbols = indexer_->ref_tracker().get_file_enhanced_symbols(file_id);

    nlohmann::json j;
    j["file_info"]["file_id"] = file_id;
    j["file_info"]["path"] = path;
    j["file_info"]["symbol_count"] = static_cast<int>(symbols.size());
    json_response(res, j);
}

// -- Endpoint: /shutdown ------------------------------------------------------

void IndexServer::handle_shutdown(const httplib::Request& /*req*/,
                                   httplib::Response& res) {
    nlohmann::json j;
    j["success"] = true;
    j["message"] = "Server shutting down";
    json_response(res, j);

    // Trigger shutdown after response is sent
    std::thread([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        {
            std::lock_guard lock(shutdown_mu_);
            shutdown_requested_ = true;
        }
        shutdown_cv_.notify_all();
    }).detach();
}

// -- Endpoint: /reindex -------------------------------------------------------

void IndexServer::handle_reindex(const httplib::Request& req,
                                  httplib::Response& res) {
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
    j["index_size_bytes"] = 0;
    j["build_duration_ms"] = stats.indexing_time_ns / 1'000'000;
    j["memory_rss_mb"] = get_rss_mb();
    j["num_threads"] = static_cast<int>(std::thread::hardware_concurrency());
    j["uptime_seconds"] = uptime;
    json_response(res, j);
}

// -- Endpoint: /definition ----------------------------------------------------

void IndexServer::handle_definition(const httplib::Request& req,
                                     httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    auto pattern = body.value("pattern", "");
    if (pattern.empty()) {
        error_response(res, 400, "pattern is required");
        return;
    }

    int max_results = body.value("max_results", 100);

    std::vector<SearchResult> results;
    {
        std::shared_lock lock(mu_);
        results = indexer_->search_definitions(pattern);
    }

    if (static_cast<int>(results.size()) > max_results) {
        results.resize(static_cast<size_t>(max_results));
    }

    nlohmann::json defs = nlohmann::json::array();
    for (const auto& r : results) {
        std::string name = r.context.block_name.empty()
                               ? r.match_text
                               : r.context.block_name;
        nlohmann::json d;
        d["name"] = name;
        d["type"] = r.context.block_type;
        d["file_path"] = r.path;
        d["line"] = r.line;
        d["column"] = r.column;
        defs.push_back(d);
    }

    nlohmann::json j;
    j["definitions"] = defs;
    json_response(res, j);
}

// -- Endpoint: /references ----------------------------------------------------

void IndexServer::handle_references(const httplib::Request& req,
                                     httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    auto pattern = body.value("pattern", "");
    if (pattern.empty()) {
        error_response(res, 400, "pattern is required");
        return;
    }

    int max_results = body.value("max_results", 100);

    // Go's /references endpoint returns one entry per text occurrence of
    // the pattern (matching the Go reference output: a definition-line
    // hit in each file). Use the general search path so the C++ output
    // matches the Go shape and result count rather than restricting to
    // recorded incoming refs (which only covers cross-language linker
    // hits and would drop the same-file definition lines).
    SearchOptions opts;
    opts.max_results = max_results;
    opts.max_context_lines = 5;

    std::vector<SearchResult> results;
    {
        std::shared_lock lock(mu_);
        results = indexer_->search_with_options(pattern, opts);
    }

    if (static_cast<int>(results.size()) > max_results) {
        results.resize(static_cast<size_t>(max_results));
    }

    nlohmann::json refs = nlohmann::json::array();
    for (const auto& r : results) {
        std::string ctx;
        if (!r.context.lines.empty()) {
            int line_idx = r.line - r.context.start_line;
            if (line_idx >= 0 &&
                line_idx < static_cast<int>(r.context.lines.size())) {
                ctx = r.context.lines[static_cast<size_t>(line_idx)];
            } else {
                ctx = r.context.lines[0];
            }
        }

        nlohmann::json rj;
        rj["file_path"] = r.path;
        rj["line"] = r.line;
        rj["column"] = r.column;
        rj["context"] = ctx;
        rj["match"] = r.match_text;
        refs.push_back(rj);
    }

    nlohmann::json j;
    j["references"] = refs;
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
    auto symbols = indexer_->ref_tracker().find_symbols_by_name(function_name);
    if (symbols.empty()) {
        nlohmann::json j;
        j["error"] = "function not found: " + function_name;
        json_response(res, j);
        return;
    }

    const auto* sym = symbols[0];
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
            node.file_id != 0 ? indexer_->get_file_path(node.file_id) : "";
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
        error_response(res, 400, "not a git repository");
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

    auto symbol_to_json = [](const git::SymbolInfo& symbol) {
        nlohmann::json out;
        out["name"] = symbol.name;
        out["type"] = symbol.type;
        out["file_path"] = symbol.file_path;
        out["line"] = symbol.line;
        if (symbol.end_line > 0) out["end_line"] = symbol.end_line;
        if (symbol.complexity > 0) out["complexity"] = symbol.complexity;
        if (symbol.lines_of_code > 0) out["lines_of_code"] = symbol.lines_of_code;
        if (symbol.nesting_depth > 0) out["nesting_depth"] = symbol.nesting_depth;
        return out;
    };

    auto metrics_to_json = [&](const git::MetricsFinding& finding) {
        nlohmann::json out;
        out["severity"] = std::string(git::to_string(finding.severity));
        out["description"] = finding.description;
        out["symbol"] = symbol_to_json(finding.symbol);
        out["issue_type"] = std::string(git::to_string(finding.issue_type));
        out["issue"] = finding.issue;
        out["suggestion"] = finding.suggestion;
        if (finding.new_metrics) {
            out["new_metrics"] = {
                {"complexity", finding.new_metrics->complexity},
                {"lines_of_code", finding.new_metrics->lines_of_code},
                {"nesting_depth", finding.new_metrics->nesting_depth},
            };
        } else {
            out["new_metrics"] = {
                {"complexity", finding.symbol.complexity},
                {"lines_of_code", finding.symbol.lines_of_code},
                {"nesting_depth", finding.symbol.nesting_depth},
            };
        }
        return out;
    };

    nlohmann::json report_j;
    report_j["summary"] = {
        {"files_changed", report.summary.files_changed},
        {"symbols_added", report.summary.symbols_added},
        {"symbols_modified", report.summary.symbols_modified},
        {"symbols_deleted", report.summary.symbols_deleted},
        {"duplicates_found", report.summary.duplicates_found},
        {"naming_issues_found", report.summary.naming_issues_found},
        {"metrics_issues_found", report.summary.metrics_issues_found},
        {"risk_score", report.summary.risk_score},
        {"top_recommendation", report.summary.top_recommendation},
    };

    if (!report.metrics_issues.empty()) {
        report_j["metrics_issues"] = nlohmann::json::array();
        for (const auto& finding : report.metrics_issues) {
            report_j["metrics_issues"].push_back(metrics_to_json(finding));
        }
    }

    auto analyzed_at = std::chrono::system_clock::to_time_t(report.metadata.analyzed_at);
    std::tm analyzed_tm{};
#ifdef _WIN32
    gmtime_s(&analyzed_tm, &analyzed_at);
#else
    gmtime_r(&analyzed_at, &analyzed_tm);
#endif
    char analyzed_buf[32];
    std::strftime(analyzed_buf, sizeof(analyzed_buf), "%Y-%m-%dT%H:%M:%SZ",
                  &analyzed_tm);

    report_j["metadata"] = {
        {"base_ref", report.metadata.base_ref},
        {"target_ref", report.metadata.target_ref},
        {"scope", std::string(git::to_string(report.metadata.scope))},
        {"analysis_time_ms", report.metadata.analysis_time_ms},
        {"analyzed_at", std::string(analyzed_buf)},
    };

    nlohmann::json j;
    j["report"] = std::move(report_j);
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
    int max_results = body.value("max", 50);
    if (max_results <= 0) max_results = 50;
    if (max_results > 500) max_results = 500;
    int offset = body.value("offset", 0);

    auto all_file_ids = indexer_->get_all_file_ids();
    // Sort by file_id ascending for deterministic output ordering.
    // get_all_file_ids() iterates a hash map and returns ids in arbitrary
    // order; without sorting, list-symbols output is non-reproducible across
    // runs and diverges from the Go reference (which iterates files in
    // insertion / file_id order).
    std::sort(all_file_ids.begin(), all_file_ids.end());

    nlohmann::json entries = nlohmann::json::array();
    int total = 0;

    for (auto fid : all_file_ids) {
        auto file_path = indexer_->get_file_path(fid);
        if (file_path.empty()) continue;

        if (!file_filter.empty()) {
            auto base = std::filesystem::path(file_path).filename().string();
            bool matched = (file_path.find(file_filter) != std::string::npos) ||
                           (base.find(file_filter) != std::string::npos);
            if (!matched) continue;
        }

        auto symbols = indexer_->ref_tracker().get_file_enhanced_symbols(fid);
        for (const auto* sym : symbols) {
            if (!kind_matches(sym->symbol.type, kinds)) continue;
            if (exported_filter.has_value()) {
                if (*exported_filter != sym->is_exported) continue;
            }
            if (!name_filter.empty() &&
                !contains_ci(sym->symbol.name, name_filter)) {
                continue;
            }
            if (!receiver_filter.empty() &&
                to_lower(sym->receiver_type) != to_lower(receiver_filter)) {
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

            ++total;

            if (total <= offset) continue;
            if (static_cast<int>(entries.size()) >= max_results) continue;

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
            if (!sym->incoming_refs.empty()) {
                e["incoming_refs"] = static_cast<int>(sym->incoming_refs.size());
            }
            if (!sym->outgoing_refs.empty()) {
                e["outgoing_refs"] = static_cast<int>(sym->outgoing_refs.size());
            }
            entries.push_back(e);
        }
    }

    nlohmann::json j;
    j["symbols"] = entries;
    j["total"] = total;
    j["showing"] = static_cast<int>(entries.size());
    j["has_more"] = total > offset + static_cast<int>(entries.size());
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

    std::vector<const EnhancedSymbol*> matched;

    // Try by ID first
    auto id_str = body.value("id", "");
    if (!id_str.empty()) {
        auto decoded = decode_symbol_id(id_str);
        if (decoded.has_value()) {
            const auto* sym =
                indexer_->ref_tracker().get_enhanced_symbol(decoded.value());
            if (sym != nullptr) {
                matched.push_back(sym);
            }
        }
    }

    // Try by name if no ID match
    auto name_str = body.value("name", "");
    if (matched.empty() && !name_str.empty()) {
        matched = indexer_->ref_tracker().find_symbols_by_name(name_str);
    }

    // Apply disambiguators (file, type)
    auto file_filter = body.value("file", "");
    auto type_filter = body.value("type", "");
    if (!file_filter.empty() || !type_filter.empty()) {
        auto type_kinds = parse_symbol_kinds(type_filter);

        std::vector<const EnhancedSymbol*> filtered;
        for (const auto* sym : matched) {
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
    for (const auto* sym : matched) {
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
        e["outgoing_refs"] = static_cast<int>(sym->outgoing_refs.size());
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
        if (!sym->incoming_refs.empty()) {
            e["incoming_refs"] = static_cast<int>(sym->incoming_refs.size());
        }
        if (!sym->annotations.empty()) {
            e["annotations"] = sym->annotations;
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
                if (auto* s = tracker.get_enhanced_symbol(id)) {
                    th["implements"].push_back(s->symbol.name);
                }
            }
            for (auto id : rels.implemented_by) {
                if (auto* s = tracker.get_enhanced_symbol(id)) {
                    th["implemented_by"].push_back(s->symbol.name);
                }
            }
            for (auto id : rels.extends) {
                if (auto* s = tracker.get_enhanced_symbol(id)) {
                    th["extends"].push_back(s->symbol.name);
                }
            }
            for (auto id : rels.extended_by) {
                if (auto* s = tracker.get_enhanced_symbol(id)) {
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

    auto symbols = indexer_->ref_tracker().get_file_enhanced_symbols(target_fid);

    nlohmann::json entries = nlohmann::json::array();
    int total = 0;
    for (const auto* sym : symbols) {
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
        if (!sym->incoming_refs.empty()) {
            e["incoming_refs"] = static_cast<int>(sym->incoming_refs.size());
        }
        if (!sym->outgoing_refs.empty()) {
            e["outgoing_refs"] = static_cast<int>(sym->outgoing_refs.size());
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

        for (const auto* sym : symbols) {
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
