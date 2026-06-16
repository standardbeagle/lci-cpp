#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/search/search_engine.h>
#include <lci/server/client.h>
#include <lci/server/server.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "test_socket.h"

#ifndef _WIN32
#include <sys/un.h>
#endif

namespace lci {
namespace {

// -- Temp directory helper (matches server_test.cpp pattern) ------------------

class TempDir {
  public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lci_client_test_" +
                 std::to_string(
                     std::hash<std::thread::id>{}(
                         std::this_thread::get_id()) ^
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

// -- Test fixture using real server -------------------------------------------

class ClientTest : public ::testing::Test {
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

        server_ = std::make_unique<IndexServer>(
            config_, *indexer_, search_engine_.get());

        socket_path_ = test::next_test_server_address();
        server_->set_socket_path(socket_path_);
        server_->set_build_id_override("test-build-id");

        ASSERT_TRUE(server_->start());

        client_ = std::make_unique<Client>(socket_path_);
        client_->set_timeout(std::chrono::milliseconds{5000});
    }

    void TearDown() override {
        client_.reset();
        if (server_ && server_->is_running()) {
            server_->shutdown();
        }
        std::error_code ec;
        std::filesystem::remove(socket_path_, ec);
    }

    TempDir tmp_;
    Config config_;
    std::unique_ptr<MasterIndex> indexer_;
    std::unique_ptr<SearchEngine> search_engine_;
    std::unique_ptr<IndexServer> server_;
    std::unique_ptr<Client> client_;
    std::string socket_path_;
};

// -- Connection tests ---------------------------------------------------------

TEST(ClientConnectionTest, ConnectToNonexistentSocket) {
    Client cli("/tmp/lci_nonexistent_socket_path.sock");
    std::string err;
    auto resp = cli.ping(err);
    EXPECT_FALSE(resp.has_value());
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("failed to connect"), std::string::npos);
}

TEST(ClientConnectionTest, IsServerRunningReturnsFalse) {
    Client cli("/tmp/lci_nonexistent_socket_path.sock");
    EXPECT_FALSE(cli.is_server_running());
}

// -- Ping tests ---------------------------------------------------------------

TEST_F(ClientTest, Ping) {
    std::string err;
    auto resp = client_->ping(err);
    ASSERT_TRUE(resp.has_value()) << err;
    EXPECT_EQ(resp->build_id_value, "test-build-id");
    EXPECT_FALSE(resp->version.empty());
    EXPECT_GE(resp->uptime_seconds, 0.0);
}

TEST_F(ClientTest, IsServerRunning) {
    EXPECT_TRUE(client_->is_server_running());
}

// -- Status tests -------------------------------------------------------------

TEST_F(ClientTest, GetStatus) {
    std::string err;
    auto status = client_->get_status(err);
    ASSERT_TRUE(status.has_value()) << err;
    EXPECT_TRUE(status->ready);
    EXPECT_GT(status->file_count, 0);
    EXPECT_GE(status->symbol_count, 0);
    EXPECT_FALSE(status->indexing_active);
}

// -- Search tests -------------------------------------------------------------

TEST_F(ClientTest, Search) {
    std::string err;
    auto j = client_->search("Add", 10, false, false, err);
    ASSERT_TRUE(j.has_value()) << err;
    EXPECT_TRUE(j->contains("results"));
    EXPECT_FALSE((*j)["results"].empty());
}

TEST_F(ClientTest, SearchEmptyPattern) {
    std::string err;
    auto j = client_->search("", 10, false, false, err);
    EXPECT_FALSE(j.has_value());
    EXPECT_FALSE(err.empty());
}

// -- Definition tests ---------------------------------------------------------

TEST_F(ClientTest, GetDefinition) {
    std::string err;
    auto defs = client_->get_definition("Add", 10, err);
    ASSERT_TRUE(defs.has_value()) << err;
    if (!defs->empty()) {
        EXPECT_FALSE((*defs)[0].name.empty());
    }
}

// -- References tests ---------------------------------------------------------

TEST_F(ClientTest, GetReferences) {
    std::string err;
    auto refs = client_->get_references("Add", 10, err);
    ASSERT_TRUE(refs.has_value()) << err;
}

// -- Stats tests --------------------------------------------------------------

TEST_F(ClientTest, GetStats) {
    std::string err;
    auto stats = client_->get_stats(err);
    ASSERT_TRUE(stats.has_value()) << err;
    EXPECT_GT(stats->file_count, 0);
    EXPECT_GE(stats->uptime_seconds, 0.0);
}

// -- Tree tests ---------------------------------------------------------------

TEST_F(ClientTest, GetTree) {
    std::string err;
    TreeRequest req;
    req.function_name = "main";
    auto j = client_->get_tree(req, err);
    // Either succeeds with a tree or the function was not found
    EXPECT_TRUE(j.has_value() || !err.empty());
}

// -- WaitForReady tests -------------------------------------------------------

TEST_F(ClientTest, WaitForReadySuccess) {
    std::string err;
    bool ready = client_->wait_for_ready(
        std::chrono::milliseconds{5000}, err);
    EXPECT_TRUE(ready) << err;
}

TEST_F(ClientTest, WaitForReadyTimeout) {
    // Shut down the server first so it will never become ready
    server_->shutdown();

    Client cli("/tmp/lci_nonexistent_wait_test.sock");
    cli.set_timeout(std::chrono::milliseconds{500});
    std::string err;
    auto start = std::chrono::steady_clock::now();
    bool ready = cli.wait_for_ready(std::chrono::milliseconds{300}, err);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(ready);
    EXPECT_NE(err.find("timeout"), std::string::npos);
    EXPECT_GE(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
        250);
}

// -- Shutdown tests -----------------------------------------------------------

TEST_F(ClientTest, Shutdown) {
    std::string err;
    bool ok = client_->shutdown(false, err);
    EXPECT_TRUE(ok) << err;
}

// -- Reindex tests ------------------------------------------------------------

TEST(ClientReindexConnectionTest, ReindexConnectionFailure) {
    Client cli("/tmp/lci_nonexistent_reindex_test.sock");
    std::string err;
    bool ok = cli.reindex("/some/path", err);
    EXPECT_FALSE(ok);
    EXPECT_NE(err.find("failed to connect"), std::string::npos);
}

// -- ListSymbols tests --------------------------------------------------------

TEST_F(ClientTest, ListSymbols) {
    std::string err;
    ListSymbolsRequest req;
    req.kind = "function";
    auto j = client_->list_symbols(req, err);
    ASSERT_TRUE(j.has_value()) << err;
    EXPECT_TRUE(j->contains("symbols"));
    EXPECT_TRUE(j->contains("total"));
}

// -- InspectSymbol tests ------------------------------------------------------

TEST_F(ClientTest, InspectSymbol) {
    std::string err;
    InspectSymbolRequest req;
    req.name = "Add";
    auto j = client_->inspect_symbol(req, err);
    ASSERT_TRUE(j.has_value()) << err;
    EXPECT_TRUE(j->contains("symbols"));
    EXPECT_TRUE(j->contains("count"));
}

// -- BrowseFile tests ---------------------------------------------------------

TEST_F(ClientTest, BrowseFile) {
    std::string err;
    BrowseFileRequest req;
    req.file = "main.go";
    auto j = client_->browse_file(req, err);
    ASSERT_TRUE(j.has_value()) << err;
    EXPECT_TRUE(j->contains("file"));
    EXPECT_TRUE(j->contains("symbols"));
}

TEST_F(ClientTest, BrowseFileNotFound) {
    std::string err;
    BrowseFileRequest req;
    req.file = "nonexistent.go";
    auto j = client_->browse_file(req, err);
    EXPECT_FALSE(j.has_value());
    EXPECT_FALSE(err.empty());
}

// -- GitAnalyze tests ---------------------------------------------------------

TEST(ClientGitAnalyzeConnectionTest, GitAnalyzeConnectionFailure) {
    Client cli("/tmp/lci_nonexistent_git_test.sock");
    std::string err;
    GitAnalyzeRequest req;
    req.scope = "staged";
    auto j = cli.git_analyze(req, err);
    EXPECT_FALSE(j.has_value());
    EXPECT_NE(err.find("failed to connect"), std::string::npos);
}

// -- Timeout tests ------------------------------------------------------------

TEST_F(ClientTest, SetTimeout) {
    client_->set_timeout(std::chrono::milliseconds{100});
    std::string err;
    auto resp = client_->ping(err);
    ASSERT_TRUE(resp.has_value()) << err;
}

// -- GetSymbol tests ----------------------------------------------------------

TEST(ClientGetSymbolConnectionTest, GetSymbolConnectionFailure) {
    Client cli("/tmp/lci_nonexistent_symbol_test.sock");
    std::string err;
    auto j = cli.get_symbol(1, err);
    EXPECT_FALSE(j.has_value());
    EXPECT_NE(err.find("failed to connect"), std::string::npos);
}

// -- GetFileInfo tests --------------------------------------------------------

TEST(ClientGetFileInfoConnectionTest, GetFileInfoConnectionFailure) {
    Client cli("/tmp/lci_nonexistent_fileinfo_test.sock");
    std::string err;
    auto j = cli.get_file_info(1, err);
    EXPECT_FALSE(j.has_value());
    EXPECT_NE(err.find("failed to connect"), std::string::npos);
}

}  // namespace
}  // namespace lci
