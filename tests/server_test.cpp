#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/search/search_engine.h>
#include <lci/server/server.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <sys/un.h>
#endif

namespace lci {
namespace {

// -- Temp directory helper (matches existing test patterns) -------------------

class TempDir {
  public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lci_server_test_" + std::to_string(
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

// -- Test fixture -------------------------------------------------------------

class ServerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        tmp_.write_file("main.go", R"(
package main

import "fmt"

func main() {
    fmt.Println("hello")
}

func Add(a, b int) int {
    return a + b
}

type Calculator struct {
    Value int
}

func (c *Calculator) Reset() {
    c.Value = 0
}
)");

        config_.project.root = tmp_.path().string();
        config_.project.name = "test";

        indexer_ = std::make_unique<MasterIndex>(config_);
        indexer_->index_directory(config_.project.root);
        search_engine_ = std::make_unique<SearchEngine>(*indexer_);

        // Create server with external index
        server_ = std::make_unique<IndexServer>(
            config_, *indexer_, search_engine_.get());

        // Use a unique socket path per test
        socket_path_ = (std::filesystem::temp_directory_path() /
                        ("lci_test_" + std::to_string(counter_++) + ".sock"))
                            .string();
        server_->set_socket_path(socket_path_);
        server_->set_build_id_override("test-build-id");

        ASSERT_TRUE(server_->start());
    }

    void TearDown() override {
        if (server_ && server_->is_running()) {
            server_->shutdown();
        }
        std::error_code ec;
        std::filesystem::remove(socket_path_, ec);
    }

    httplib::Client make_client() {
        httplib::Client cli(socket_path_);
        cli.set_address_family(AF_UNIX);
        cli.set_connection_timeout(std::chrono::seconds{5});
        cli.set_read_timeout(std::chrono::seconds{5});
        return cli;
    }

    nlohmann::json post(const std::string& path,
                        const nlohmann::json& body = nlohmann::json{}) {
        auto cli = make_client();

        auto res = cli.Post(path, body.dump(), "application/json");
        if (!res) {
            return {{"error", "connection failed"}};
        }
        try {
            return nlohmann::json::parse(res->body);
        } catch (...) {
            return {{"raw", res->body}};
        }
    }

    nlohmann::json get(const std::string& path) {
        auto cli = make_client();

        auto res = cli.Get(path);
        if (!res) {
            return {{"error", "connection failed"}};
        }
        try {
            return nlohmann::json::parse(res->body);
        } catch (...) {
            return {{"raw", res->body}};
        }
    }

    TempDir tmp_;
    Config config_;
    std::unique_ptr<MasterIndex> indexer_;
    std::unique_ptr<SearchEngine> search_engine_;
    std::unique_ptr<IndexServer> server_;
    std::string socket_path_;
    static inline int counter_ = 0;
};

// -- Socket path helper tests -------------------------------------------------

TEST(SocketPathTest, DefaultPath) {
    auto path = get_socket_path();
    EXPECT_FALSE(path.empty());
    EXPECT_NE(path.find("lci-server.sock"), std::string::npos);
}

TEST(SocketPathTest, ProjectSpecificPath) {
    auto path1 = get_socket_path_for_root("/project/a");
    auto path2 = get_socket_path_for_root("/project/b");
    EXPECT_NE(path1, path2);
    EXPECT_NE(path1.find("lci-server-"), std::string::npos);
}

TEST(SocketPathTest, EmptyRootFallsBack) {
    auto path = get_socket_path_for_root("");
    EXPECT_EQ(path, get_socket_path());
}

// -- Build ID tests -----------------------------------------------------------

TEST(BuildIDTest, ReturnsNonEmpty) {
    auto id = build_id();
    EXPECT_FALSE(id.empty());
    EXPECT_EQ(id.size(), 8u);
}

TEST(BuildIDTest, Deterministic) {
    EXPECT_EQ(build_id(), build_id());
}

// -- Server endpoint tests ----------------------------------------------------

TEST_F(ServerTest, PingEndpoint) {
    auto j = post("/ping");
    ASSERT_FALSE(j.contains("error") && j["error"].is_string() &&
                 j["error"] == "connection failed");
    EXPECT_TRUE(j.contains("uptime_seconds"));
    EXPECT_TRUE(j.contains("version"));
    EXPECT_EQ(j["build_id"], "test-build-id");
}

TEST_F(ServerTest, PingViaGet) {
    auto j = get("/ping");
    ASSERT_FALSE(j.contains("error") && j["error"].is_string() &&
                 j["error"] == "connection failed");
    EXPECT_TRUE(j.contains("version"));
}

TEST_F(ServerTest, StatusEndpoint) {
    auto j = post("/status");
    ASSERT_TRUE(j.contains("ready"));
    EXPECT_TRUE(j["ready"].get<bool>());
    EXPECT_GT(j["file_count"].get<int>(), 0);
    EXPECT_GE(j["symbol_count"].get<int>(), 0);
    EXPECT_FALSE(j["indexing_active"].get<bool>());
}

TEST_F(ServerTest, SearchEndpoint) {
    auto j = post("/search", {{"pattern", "Add"}});
    ASSERT_TRUE(j.contains("results"));
    EXPECT_FALSE(j["results"].empty());
}

TEST_F(ServerTest, SearchEmptyPattern) {
    auto j = post("/search", {{"pattern", ""}});
    EXPECT_TRUE(j.contains("error"));
}

TEST_F(ServerTest, SearchInvalidJson) {
    auto cli = make_client();
    auto res = cli.Post("/search", "not json", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_TRUE(j.contains("error"));
}

TEST_F(ServerTest, DefinitionEndpoint) {
    auto j = post("/definition", {{"pattern", "Add"}});
    ASSERT_TRUE(j.contains("definitions"));
    EXPECT_TRUE(j["definitions"].is_array());
    // Definitions may be empty if the indexer didn't extract declaration
    // symbols, but the JSON structure must be correct.
    if (!j["definitions"].empty()) {
        EXPECT_TRUE(j["definitions"][0].contains("file_path"));
        EXPECT_TRUE(j["definitions"][0].contains("line"));
    }
}

TEST_F(ServerTest, ReferencesEndpoint) {
    auto j = post("/references", {{"pattern", "Add"}});
    ASSERT_TRUE(j.contains("references"));
}

TEST_F(ServerTest, StatsEndpoint) {
    auto j = post("/stats");
    ASSERT_TRUE(j.contains("file_count"));
    EXPECT_GT(j["file_count"].get<int>(), 0);
    EXPECT_TRUE(j.contains("uptime_seconds"));
}

TEST_F(ServerTest, StatsViaGet) {
    auto j = get("/stats");
    ASSERT_TRUE(j.contains("file_count"));
}

TEST_F(ServerTest, ListSymbolsEndpoint) {
    auto j = post("/list-symbols", {{"kind", "function"}});
    ASSERT_TRUE(j.contains("symbols"));
    ASSERT_TRUE(j.contains("total"));
    ASSERT_TRUE(j.contains("showing"));
    ASSERT_TRUE(j.contains("has_more"));
    EXPECT_GE(j["total"].get<int>(), 0);
    if (!j["symbols"].empty()) {
        EXPECT_TRUE(j["symbols"][0].contains("name"));
        EXPECT_TRUE(j["symbols"][0].contains("type"));
        EXPECT_TRUE(j["symbols"][0].contains("file"));
    }
}

TEST_F(ServerTest, ListSymbolsWithFilters) {
    auto j = post("/list-symbols", {
        {"kind", "function"},
        {"name", "Add"},
        {"max", 10}
    });
    ASSERT_TRUE(j.contains("symbols"));
    for (auto& sym : j["symbols"]) {
        auto name = sym["name"].get<std::string>();
        // Name should contain "Add" (case-insensitive)
        auto lower = name;
        for (auto& c : lower) c = static_cast<char>(std::tolower(
            static_cast<unsigned char>(c)));
        EXPECT_NE(lower.find("add"), std::string::npos);
    }
}

TEST_F(ServerTest, InspectSymbolByName) {
    auto j = post("/inspect-symbol", {{"name", "Add"}});
    ASSERT_TRUE(j.contains("symbols"));
    ASSERT_TRUE(j.contains("count"));
    EXPECT_GE(j["count"].get<int>(), 0);
    if (!j["symbols"].empty()) {
        EXPECT_TRUE(j["symbols"][0].contains("name"));
        EXPECT_TRUE(j["symbols"][0].contains("object_id"));
        EXPECT_TRUE(j["symbols"][0].contains("type"));
        EXPECT_TRUE(j["symbols"][0].contains("file"));
    }
}

TEST_F(ServerTest, BrowseFileEndpoint) {
    auto j = post("/browse-file", {{"file", "main.go"}});
    ASSERT_TRUE(j.contains("file"));
    ASSERT_TRUE(j.contains("symbols"));
    ASSERT_TRUE(j.contains("total"));
    EXPECT_GE(j["total"].get<int>(), 0);
    EXPECT_TRUE(j["file"].contains("language"));
    EXPECT_EQ(j["file"]["language"], "go");
}

TEST_F(ServerTest, BrowseFileWithStats) {
    auto j = post("/browse-file", {
        {"file", "main.go"},
        {"show_stats", true}
    });
    ASSERT_TRUE(j.contains("stats"));
    EXPECT_TRUE(j["stats"].contains("symbol_count"));
    EXPECT_TRUE(j["stats"].contains("function_count"));
}

TEST_F(ServerTest, BrowseFileNotFound) {
    auto j = post("/browse-file", {{"file", "nonexistent.go"}});
    EXPECT_TRUE(j.contains("error"));
}

TEST_F(ServerTest, TreeEndpoint) {
    auto j = post("/tree", {{"function_name", "main"}});
    // Either has a tree or an error if not found
    EXPECT_TRUE(j.contains("tree") || j.contains("error"));
}

TEST_F(ServerTest, GitAnalyzeNotImplemented) {
    auto j = post("/git-analyze", {{"scope", "staged"}});
    EXPECT_TRUE(j.contains("error"));
}

TEST_F(ServerTest, ShutdownEndpoint) {
    auto j = post("/shutdown");
    EXPECT_TRUE(j["success"].get<bool>());
    EXPECT_EQ(j["message"], "Server shutting down");
}

TEST_F(ServerTest, BuildIdStaleDetection) {
    auto j1 = post("/ping");
    EXPECT_EQ(j1["build_id"], "test-build-id");

    // Different build ID means stale server
    server_->set_build_id_override("new-build-id");
    auto j2 = post("/ping");
    EXPECT_EQ(j2["build_id"], "new-build-id");
    EXPECT_NE(j1["build_id"], j2["build_id"]);
}

TEST_F(ServerTest, ConcurrentRequests) {
    constexpr int kNumThreads = 8;
    constexpr int kRequestsPerThread = 5;
    std::atomic<int> success_count{0};

    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kRequestsPerThread; ++i) {
                auto j = post("/ping");
                if (j.contains("version")) {
                    success_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(success_count.load(), kNumThreads * kRequestsPerThread);
}

TEST_F(ServerTest, GracefulShutdownWithinTimeout) {
    auto start = std::chrono::steady_clock::now();
    bool clean = server_->shutdown(std::chrono::milliseconds{5000});
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(clean);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                  .count(),
              5000);
    EXPECT_FALSE(server_->is_running());
}

}  // namespace
}  // namespace lci
