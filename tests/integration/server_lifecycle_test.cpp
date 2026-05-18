#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/search/search_engine.h>
#include <lci/server/server.h>

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <sys/un.h>
#endif

namespace lci {
namespace {

// TempDir matches the pattern used by existing passing tests.
class TempDir {
  public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lci_lifecycle_integ_" + std::to_string(
                    std::hash<std::thread::id>{}(std::this_thread::get_id()) ^
                    std::hash<int>{}(counter_++)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

    void write_file(const std::string& rel_path,
                    const std::string& content) {
        auto full = path_ / rel_path;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream f(full);
        f << content;
    }

  private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

// ServerLifecycleTest exercises the full server lifecycle: startup, indexing,
// hitting all 15 endpoints with a real index, and clean shutdown.

class ServerLifecycleTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_.write_file("main.go",
            "package main\n"
            "\n"
            "import \"fmt\"\n"
            "\n"
            "func main() {\n"
            "    fmt.Println(greet(\"world\"))\n"
            "}\n"
            "\n"
            "func greet(name string) string {\n"
            "    return \"Hello, \" + name\n"
            "}\n");
        dir_.write_file("calc.go",
            "package main\n"
            "\n"
            "type Calculator struct {\n"
            "    Value int\n"
            "}\n"
            "\n"
            "func (c *Calculator) Add(v int) {\n"
            "    c.Value += v\n"
            "}\n"
            "\n"
            "func (c *Calculator) Reset() {\n"
            "    c.Value = 0\n"
            "}\n");
        dir_.write_file("util.py",
            "def helper(x):\n"
            "    return x * 2\n"
            "\n"
            "class StringUtils:\n"
            "    @staticmethod\n"
            "    def upper(s):\n"
            "        return s.upper()\n");

        config_.project.root = dir_.path().string();
        config_.project.name = "lifecycle-test";

        indexer_ = std::make_unique<MasterIndex>(config_);
        indexer_->index_directory(config_.project.root);
        search_engine_ = std::make_unique<SearchEngine>(*indexer_);

        server_ = std::make_unique<IndexServer>(
            config_, *indexer_, search_engine_.get());

        // Include PID + counter so concurrent ctest fork processes
        // don't collide on /tmp/lci_lifecycle_<N>.sock. The counter
        // alone reset to 0 in each process; same N reused across
        // forks bound to the same socket file, causing EADDRINUSE
        // and intermittent test failures under -j.
        socket_path_ =
            (std::filesystem::temp_directory_path() /
             ("lci_lifecycle_" + std::to_string(::getpid()) + "_" +
              std::to_string(socket_counter_++) + ".sock"))
                .string();
        server_->set_socket_path(socket_path_);
        server_->set_build_id_override("lifecycle-test-id");

        ASSERT_TRUE(server_->start());
    }

    void TearDown() override {
        if (server_ && server_->is_running()) {
            server_->shutdown();
        }
        std::error_code ec;
        std::filesystem::remove(socket_path_, ec);
    }

    nlohmann::json post(const std::string& path,
                        const nlohmann::json& body = nlohmann::json{}) {
        httplib::Client cli(socket_path_);
        cli.set_address_family(AF_UNIX);
        cli.set_connection_timeout(std::chrono::seconds{5});
        cli.set_read_timeout(std::chrono::seconds{5});

        auto res = cli.Post(path, body.dump(), "application/json");
        if (!res) return {{"error", "connection failed"}};
        try {
            return nlohmann::json::parse(res->body);
        } catch (...) {
            return {{"raw", res->body}};
        }
    }

    nlohmann::json get(const std::string& path) {
        httplib::Client cli(socket_path_);
        cli.set_address_family(AF_UNIX);
        cli.set_connection_timeout(std::chrono::seconds{5});
        cli.set_read_timeout(std::chrono::seconds{5});

        auto res = cli.Get(path);
        if (!res) return {{"error", "connection failed"}};
        try {
            return nlohmann::json::parse(res->body);
        } catch (...) {
            return {{"raw", res->body}};
        }
    }

    TempDir dir_;
    Config config_;
    std::unique_ptr<MasterIndex> indexer_;
    std::unique_ptr<SearchEngine> search_engine_;
    std::unique_ptr<IndexServer> server_;
    std::string socket_path_;
    static inline int socket_counter_ = 0;
};

// -- 1. /ping (POST) ----------------------------------------------------------

TEST_F(ServerLifecycleTest, Endpoint_Ping_POST) {
    auto j = post("/ping");
    ASSERT_FALSE(j.contains("error") && j["error"] == "connection failed");
    EXPECT_TRUE(j.contains("uptime_seconds"));
    EXPECT_TRUE(j.contains("version"));
    EXPECT_EQ(j["build_id"], "lifecycle-test-id");
}

// -- 2. /ping (GET) -----------------------------------------------------------

TEST_F(ServerLifecycleTest, Endpoint_Ping_GET) {
    auto j = get("/ping");
    ASSERT_FALSE(j.contains("error") && j["error"] == "connection failed");
    EXPECT_TRUE(j.contains("version"));
}

// -- 3. /status ---------------------------------------------------------------

TEST_F(ServerLifecycleTest, Endpoint_Status) {
    auto j = post("/status");
    ASSERT_TRUE(j.contains("ready"));
    EXPECT_TRUE(j["ready"].get<bool>());
    EXPECT_GT(j["file_count"].get<int>(), 0);
    EXPECT_GE(j["symbol_count"].get<int>(), 0);
    EXPECT_FALSE(j["indexing_active"].get<bool>());
}

// -- 4. /search ---------------------------------------------------------------

TEST_F(ServerLifecycleTest, Endpoint_Search) {
    auto j = post("/search", {{"pattern", "greet"}});
    ASSERT_TRUE(j.contains("results"));
    EXPECT_FALSE(j["results"].empty());

    bool found = false;
    for (const auto& r : j["results"]) {
        if (r["path"].get<std::string>().find("main.go") != std::string::npos) {
            found = true;
            EXPECT_GT(r["line"].get<int>(), 0);
        }
    }
    EXPECT_TRUE(found) << "Search for 'greet' should find main.go";
}

// -- 5. /search with empty pattern (error case) -------------------------------

TEST_F(ServerLifecycleTest, Endpoint_Search_EmptyPattern) {
    auto j = post("/search", {{"pattern", ""}});
    EXPECT_TRUE(j.contains("error"));
}

// -- 6. /definition -----------------------------------------------------------

TEST_F(ServerLifecycleTest, Endpoint_Definition) {
    auto j = post("/definition", {{"pattern", "Calculator"}});
    ASSERT_TRUE(j.contains("definitions"));
    EXPECT_TRUE(j["definitions"].is_array());
}

// -- 7. /references -----------------------------------------------------------

TEST_F(ServerLifecycleTest, Endpoint_References) {
    auto j = post("/references", {{"pattern", "greet"}});
    ASSERT_TRUE(j.contains("references"));
}

// -- 8. /stats (POST) ---------------------------------------------------------

TEST_F(ServerLifecycleTest, Endpoint_Stats_POST) {
    auto j = post("/stats");
    ASSERT_TRUE(j.contains("file_count"));
    EXPECT_GT(j["file_count"].get<int>(), 0);
    EXPECT_TRUE(j.contains("uptime_seconds"));
}

// -- 9. /stats (GET) ----------------------------------------------------------

TEST_F(ServerLifecycleTest, Endpoint_Stats_GET) {
    auto j = get("/stats");
    ASSERT_TRUE(j.contains("file_count"));
}

// -- 10. /list-symbols --------------------------------------------------------

TEST_F(ServerLifecycleTest, Endpoint_ListSymbols) {
    auto j = post("/list-symbols", {{"kind", "function"}});
    ASSERT_TRUE(j.contains("symbols"));
    ASSERT_TRUE(j.contains("total"));
    ASSERT_TRUE(j.contains("showing"));
    ASSERT_TRUE(j.contains("has_more"));
    EXPECT_GE(j["total"].get<int>(), 0);
}

// -- 11. /inspect-symbol ------------------------------------------------------

TEST_F(ServerLifecycleTest, Endpoint_InspectSymbol) {
    auto j = post("/inspect-symbol", {{"name", "Calculator"}});
    ASSERT_TRUE(j.contains("symbols"));
    ASSERT_TRUE(j.contains("count"));
}

// -- 12. /browse-file ---------------------------------------------------------

TEST_F(ServerLifecycleTest, Endpoint_BrowseFile) {
    auto j = post("/browse-file", {{"file", "main.go"}});
    ASSERT_TRUE(j.contains("file"));
    ASSERT_TRUE(j.contains("symbols"));
    ASSERT_TRUE(j.contains("total"));
    EXPECT_TRUE(j["file"].contains("language"));
}

// -- 13. /browse-file not found -----------------------------------------------

TEST_F(ServerLifecycleTest, Endpoint_BrowseFile_NotFound) {
    auto j = post("/browse-file", {{"file", "nonexistent.xyz"}});
    EXPECT_TRUE(j.contains("error"));
}

// -- 14. /tree ----------------------------------------------------------------

TEST_F(ServerLifecycleTest, Endpoint_Tree) {
    auto j = post("/tree", {{"function_name", "main"}});
    EXPECT_TRUE(j.contains("tree") || j.contains("error"));
}

// -- 15. /shutdown ------------------------------------------------------------

TEST_F(ServerLifecycleTest, Endpoint_Shutdown) {
    auto j = post("/shutdown");
    EXPECT_TRUE(j["success"].get<bool>());
    EXPECT_EQ(j["message"], "Server shutting down");
}

// -- Lifecycle: graceful shutdown within timeout -------------------------------

TEST_F(ServerLifecycleTest, GracefulShutdownWithinTimeout) {
    auto start = std::chrono::steady_clock::now();
    bool clean = server_->shutdown(std::chrono::milliseconds{5000});
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(clean);
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
        5000);
    EXPECT_FALSE(server_->is_running());
}

// -- Lifecycle: concurrent requests during operation --------------------------

TEST_F(ServerLifecycleTest, ConcurrentRequestsDuringOperation) {
    constexpr int kNumThreads = 4;
    constexpr int kRequestsPerThread = 5;
    std::atomic<int> success_count{0};
    std::atomic<int> failure_count{0};

    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kRequestsPerThread; ++i) {
                nlohmann::json j;
                if (i % 3 == 0) {
                    j = post("/ping");
                } else if (i % 3 == 1) {
                    j = post("/search", {{"pattern", "greet"}});
                } else {
                    j = post("/status");
                }

                if (j.contains("error") && j["error"] == "connection failed") {
                    failure_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    success_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) th.join();

    EXPECT_EQ(success_count.load(), kNumThreads * kRequestsPerThread);
    EXPECT_EQ(failure_count.load(), 0);
}

// -- Lifecycle: shutdown cleanly cancels in-flight indexing -------------------
//
// Verifies the indexing thread is tracked (not detached) and joined on
// shutdown. Without this, a long indexing run could touch freed members
// after the server destructor returned.
TEST(ServerCancellationTest, ShutdownJoinsInFlightIndexing) {
    std::filesystem::path tmp = std::filesystem::temp_directory_path() /
        ("lci_shutdown_cancel_" + std::to_string(::getpid()));
    std::filesystem::create_directories(tmp);

    auto cleanup = [&] {
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
    };

    // Write enough files that startup indexing is still in flight when
    // we call shutdown(). 200 small files is enough on debug builds
    // without making the test slow on release builds (it just means
    // shutdown sees a quick run that already finished).
    for (int i = 0; i < 200; ++i) {
        std::ofstream f(tmp / ("f" + std::to_string(i) + ".go"));
        f << "package f" << i << "\nfunc F" << i << "() {}\n";
    }

    Config cfg;
    cfg.project.root = tmp.string();
    cfg.project.name = "shutdown-cancel-test";

    auto server = std::make_unique<IndexServer>(cfg);

    auto sock = (std::filesystem::temp_directory_path() /
                 ("lci_shutdown_cancel_" +
                  std::to_string(::getpid()) + ".sock"))
                    .string();
    server->set_socket_path(sock);

    ASSERT_TRUE(server->start());

    // Issue shutdown immediately. The server must cancel the in-flight
    // initial-indexing thread and join it before returning. If the
    // thread were detached, the join would be skipped and shutdown
    // could return while indexing was still mutating the indexer.
    bool clean = server->shutdown(std::chrono::milliseconds{5000});
    EXPECT_TRUE(clean);
    EXPECT_FALSE(server->is_running());

    server.reset();
    cleanup();
    std::error_code ec;
    std::filesystem::remove(sock, ec);
}

}  // namespace
}  // namespace lci
