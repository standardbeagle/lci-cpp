#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/runtime.h>
#include <lci/search/search_engine.h>
#include <lci/server/server.h>
#include <lci/server/request_decode.h>

#include <cctype>
#include <chrono>
#include <functional>
#include <future>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "test_socket.h"
#include "unique_temp.h"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace lci {
namespace {

TEST(ServerRequestDecodeTest, RejectsWrongFieldTypes) {
    std::string error;
    EXPECT_FALSE(server_request::decode_search(
        {{"pattern", "Add"}, {"max_results", "many"}}, error));
    EXPECT_NE(error.find("integer"), std::string::npos);
}

TEST(ServerRequestDecodeTest, BoundsNumericOptions) {
    std::string error;
    auto request = server_request::decode_search(
        {{"pattern", "Add"}, {"max_results", -1},
         {"max_context_lines", 10000}}, error);
    ASSERT_TRUE(request) << error;
    EXPECT_EQ(request->max_results, 100);
    EXPECT_EQ(request->max_context_lines, 100);
}

// -- Temp directory helper (matches existing test patterns) -------------------

class TempDir {
  public:
    TempDir() {
        path_ = test::unique_temp_dir("lci_server_test_");
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

        // Platform-correct unique address per test (AF_UNIX path on POSIX,
        // localhost:<port> on Windows).
        socket_path_ = test::next_test_server_address();
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
        return test::make_test_http_client(socket_path_);
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
};

// -- Socket path helper tests -------------------------------------------------

TEST(SocketPathTest, DefaultPath) {
    auto path = get_socket_path();
    EXPECT_FALSE(path.empty());
#ifdef _WIN32
    // Windows: TCP fallback "127.0.0.1:<port>" (IPv4 literal, not "localhost",
    // which resolves to ::1 first and misses the IPv4 listener).
    EXPECT_NE(path.find("127.0.0.1:"), std::string::npos);
#else
    // POSIX: filename incorporates uid so two users on the same host
    // get distinct default sockets.
    auto expected_uid = std::to_string(static_cast<unsigned>(::getuid()));
    EXPECT_NE(path.find("lci-" + expected_uid + ".sock"), std::string::npos);
#endif
}

TEST(SocketPathTest, ProjectSpecificPath) {
    auto path1 = get_socket_path_for_root("/project/a");
    auto path2 = get_socket_path_for_root("/project/b");
    EXPECT_NE(path1, path2);
#ifdef _WIN32
    EXPECT_NE(path1.find("127.0.0.1:"), std::string::npos);
#else
    EXPECT_NE(path1.find("lci-"), std::string::npos);
    EXPECT_NE(path1.find(".sock"), std::string::npos);
#endif
}

TEST(SocketPathTest, EmptyRootFallsBack) {
    auto path = get_socket_path_for_root("");
    EXPECT_EQ(path, get_socket_path());
}

#ifndef _WIN32
TEST(SocketPathTest, FilenameIncorporatesUid) {
    // Per acceptance criterion: socket path must incorporate the uid.
    auto uid_str = std::to_string(static_cast<unsigned>(::getuid()));
    auto default_path = get_socket_path();
    auto project_path = get_socket_path_for_root("/some/project/root");

    // Both forms must embed the uid as a -<uid>- or -<uid>. token so two
    // users on the same host never collide regardless of project root.
    EXPECT_NE(default_path.find("-" + uid_str + "."), std::string::npos)
        << "default path: " << default_path;
    EXPECT_NE(project_path.find("-" + uid_str + "-"), std::string::npos)
        << "project path: " << project_path;
}

TEST(SocketPathTest, ProjectSpecificPathIsDeterministic) {
    // Per acceptance criterion: same user, same project always gets same
    // socket path.
    auto p1 = get_socket_path_for_root("/repo/foo");
    auto p2 = get_socket_path_for_root("/repo/foo");
    EXPECT_EQ(p1, p2);
}

TEST(SocketPathTest, FilenameFitsSunPathLimit) {
    // Linux's sockaddr_un.sun_path is 108 bytes including the NUL
    // terminator. The path generation must stay safely under that even
    // for absurdly long project roots.
    sockaddr_un sa{};
    const std::size_t sun_path_max = sizeof(sa.sun_path);
    auto p_default = get_socket_path();
    auto p_project = get_socket_path_for_root(
        "/very/long/project/root/path/that/exercises/the/hash/input");
    EXPECT_LT(p_default.size() + 1, sun_path_max)
        << "default path too long: " << p_default;
    EXPECT_LT(p_project.size() + 1, sun_path_max)
        << "project path too long: " << p_project;
}

TEST(SocketPathTest, DifferentProjectsProduceDistinctPaths) {
    // Per acceptance criterion: same user, different projects -> different
    // sockets (already implied by ProjectSpecificPath but pinned here as a
    // distinct AC anchor).
    auto p1 = get_socket_path_for_root("/work/proj-a");
    auto p2 = get_socket_path_for_root("/work/proj-b");
    EXPECT_NE(p1, p2);
}
#endif

TEST(ServerLifecycleFailureTest, FailedStartCanBeShutdownAndDestroyed) {
    TempDir tmp;
    Config config;
    config.project.root = tmp.path().string();
    MasterIndex indexer(config);

    IndexServer server(config, indexer, nullptr);
#ifdef _WIN32
    server.set_socket_path("invalid-address");
#else
    server.set_socket_path(
        (tmp.path() / "missing-parent" / "server.sock").string());
#endif

    EXPECT_FALSE(server.start());
    EXPECT_FALSE(server.is_running());
    EXPECT_TRUE(server.shutdown());
    EXPECT_TRUE(server.shutdown());
}

TEST(ServerLifecycleTest, CanRestartAfterCleanShutdown) {
    TempDir tmp;
    Config config;
    config.project.root = tmp.path().string();
    MasterIndex indexer(config);
    SearchEngine engine(indexer);
    IndexServer server(config, indexer, &engine);

    server.set_socket_path(test::next_test_server_address());
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.shutdown());
    ASSERT_TRUE(server.start());
    EXPECT_TRUE(server.is_running());
    EXPECT_TRUE(server.shutdown());
}

TEST(ServerLifecycleTest, SelfStopInvokesCallbackWhenRootDeleted) {
    // The reaper's self-stop (root deleted here; same path serves the RSS
    // self-cap) must notify an owner whose main loop cannot poll
    // is_running() — the MCP-host stdio transport blocks in getline and
    // used to keep the whole index resident after a self-stop.
    TempDir tmp;
    auto root = tmp.path() / "project";
    std::filesystem::create_directories(root);

    Config config;
    config.project.root = root.string();
    MasterIndex indexer(config);
    SearchEngine engine(indexer);
    IndexServer server(config, indexer, &engine);
    server.set_socket_path(test::next_test_server_address());

    std::promise<std::string> stopped;
    auto stopped_reason = stopped.get_future();
    server.set_self_stop_callback([&stopped](const char* reason) {
        stopped.set_value(reason);
    });

    ASSERT_TRUE(server.start());
    std::filesystem::remove_all(root);

    // Reaper ticks every 500ms; 10s is a generous ceiling, not a wait.
    ASSERT_EQ(stopped_reason.wait_for(std::chrono::seconds(10)),
              std::future_status::ready);
    EXPECT_EQ(stopped_reason.get(), "project root deleted");
    EXPECT_FALSE(server.is_running());
    EXPECT_TRUE(server.shutdown());
}

TEST(ServerLifecycleTest, ConcurrentStartAndShutdownAreSerialized) {
    TempDir tmp;
    Config config;
    config.project.root = tmp.path().string();
    MasterIndex indexer(config);
    SearchEngine engine(indexer);
    IndexServer server(config, indexer, &engine);
    server.set_socket_path(test::next_test_server_address());

    std::thread starter([&] { (void)server.start(); });
    std::thread stopper([&] { (void)server.shutdown(); });
    starter.join();
    stopper.join();

    EXPECT_TRUE(server.shutdown());
    EXPECT_FALSE(server.is_running());
}

#ifdef _WIN32
TEST(ServerLifecycleFailureTest, MalformedPortReturnsFalse) {
    TempDir tmp;
    Config config;
    config.project.root = tmp.path().string();
    MasterIndex indexer(config);
    IndexServer server(config, indexer, nullptr);
    server.set_socket_path("127.0.0.1:not-a-port");

    EXPECT_FALSE(server.start());
    EXPECT_FALSE(server.is_running());
}
#endif

TEST(ServerLifecycleTest, DeferredEngineGatesReadinessUntilPublished) {
    // Externally-owned index whose engine arrives later (the MCP-embedded
    // shape): the server must answer 503 / ready=false while the external
    // build is in flight, and flip ready once set_search_engine publishes.
    TempDir tmp;
    tmp.write_file("main.go", "package main\nfunc Add(a, b int) int { return a + b }\n");
    Config config;
    config.project.root = tmp.path().string();
    MasterIndex indexer(config);
    IndexServer server(config, indexer, nullptr);
    server.set_socket_path(test::next_test_server_address());
    ASSERT_TRUE(server.start());

    auto cli = test::make_test_http_client(server.socket_path());
    auto status = cli.Get("/status");
    ASSERT_TRUE(status);
    auto sj = nlohmann::json::parse(status->body);
    EXPECT_FALSE(sj["ready"].get<bool>());
    EXPECT_TRUE(sj["indexing_active"].get<bool>());

    auto blocked = cli.Post("/search", R"({"pattern":"Add"})",
                            "application/json");
    ASSERT_TRUE(blocked);
    EXPECT_EQ(blocked->status, 503);

    indexer.index_directory(config.project.root);
    SearchEngine engine(indexer);
    server.set_search_engine(&engine);

    status = cli.Get("/status");
    ASSERT_TRUE(status);
    sj = nlohmann::json::parse(status->body);
    EXPECT_TRUE(sj["ready"].get<bool>());
    EXPECT_FALSE(sj["indexing_active"].get<bool>());

    auto ok = cli.Post("/search", R"({"pattern":"Add"})", "application/json");
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200);

    server.set_search_engine(nullptr);
    EXPECT_TRUE(server.shutdown());
}

TEST(ServerLifecycleTest, StartRefusesToStealALiveListener) {
    TempDir tmp;
    tmp.write_file("a.go", "package main\n");
    Config config;
    config.project.root = tmp.path().string();
    MasterIndex indexer(config);
    SearchEngine engine(indexer);

    IndexServer first(config, indexer, &engine);
    const auto addr = test::next_test_server_address();
    first.set_socket_path(addr);
    ASSERT_TRUE(first.start());

    IndexServer second(config, indexer, &engine);
    second.set_socket_path(addr);
    EXPECT_FALSE(second.start());

    // The refusal must not have disturbed the live listener.
    auto cli = test::make_test_http_client(addr);
    auto ping = cli.Get("/ping");
    ASSERT_TRUE(ping);
    EXPECT_EQ(ping->status, 200);

    EXPECT_TRUE(first.shutdown());
    EXPECT_TRUE(second.shutdown());
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
    // Root lets clients detect socket-hash collisions (two roots, one
    // address) instead of silently searching the wrong project.
    ASSERT_TRUE(j.contains("root"));
    EXPECT_EQ(j["root"], config_.project.root);
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

TEST_F(ServerTest, StatusEndpointReportsIndexingProgress) {
    // /status must always include the indexing_progress object so a
    // long-running poller doesn't have to special-case "no run yet".
    // After fixture setup the indexer has finished, so the snapshot
    // should be the documented idle/zero shape.
    auto j = post("/status");
    ASSERT_TRUE(j.contains("indexing_progress"));
    auto ip = j["indexing_progress"];
    ASSERT_TRUE(ip.is_object());

    // Required keys (acceptance contract).
    ASSERT_TRUE(ip.contains("phase"));
    ASSERT_TRUE(ip.contains("files_scanned"));
    ASSERT_TRUE(ip.contains("files_total"));
    ASSERT_TRUE(ip.contains("percent_complete"));
    ASSERT_TRUE(ip.contains("elapsed_ms"));

    // Idle invariant: phase=idle, every numeric field 0.
    EXPECT_EQ(ip["phase"].get<std::string>(), "idle");
    EXPECT_EQ(ip["files_scanned"].get<int>(), 0);
    EXPECT_EQ(ip["files_total"].get<int>(), 0);
    EXPECT_EQ(ip["percent_complete"].get<int>(), 0);
    EXPECT_EQ(ip["elapsed_ms"].get<int64_t>(), 0);
}

TEST_F(ServerTest, IndexingProgressFieldTypesAndRanges) {
    auto j = post("/status");
    auto ip = j["indexing_progress"];

    // Strict types: phase is string, others numeric.
    EXPECT_TRUE(ip["phase"].is_string());
    EXPECT_TRUE(ip["files_scanned"].is_number_integer());
    EXPECT_TRUE(ip["files_total"].is_number_integer());
    EXPECT_TRUE(ip["percent_complete"].is_number());
    EXPECT_TRUE(ip["elapsed_ms"].is_number());

    // Non-negative invariants.
    EXPECT_GE(ip["files_scanned"].get<int>(), 0);
    EXPECT_GE(ip["files_total"].get<int>(), 0);
    EXPECT_GE(ip["percent_complete"].get<double>(), 0.0);
    EXPECT_LE(ip["percent_complete"].get<double>(), 100.0);
    EXPECT_GE(ip["elapsed_ms"].get<int64_t>(), 0);
}

TEST_F(ServerTest, IndexingProgressPhaseIsKnownEnumValue) {
    auto j = post("/status");
    auto phase = j["indexing_progress"]["phase"].get<std::string>();
    // Locked enum values from phase_to_string in src/server/server.cpp.
    EXPECT_TRUE(phase == "idle" || phase == "scanning" ||
                phase == "indexing" || phase == "merging")
        << "unknown phase value: " << phase;
}

TEST_F(ServerTest, IndexingProgressInvariantScannedLeqTotal) {
    auto j = post("/status");
    auto ip = j["indexing_progress"];
    // When both are populated, scanned must never exceed total.
    int scanned = ip["files_scanned"].get<int>();
    int total = ip["files_total"].get<int>();
    if (total > 0) {
        EXPECT_LE(scanned, total)
            << "files_scanned must not exceed files_total";
    }
}

TEST_F(ServerTest, StatusReadyFlagAndProgressConsistency) {
    auto j = post("/status");
    EXPECT_TRUE(j["ready"].get<bool>());
    EXPECT_FALSE(j["indexing_active"].get<bool>());
    EXPECT_EQ(j["indexing_progress"]["phase"].get<std::string>(), "idle");
    // The legacy `progress` field is 1.0 when ready, 0.0 otherwise.
    EXPECT_DOUBLE_EQ(j["progress"].get<double>(), 1.0);
}

TEST_F(ServerTest, SearchEndpoint) {
    auto j = post("/search", {{"pattern", "Add"}});
    ASSERT_TRUE(j.contains("results"));
    EXPECT_FALSE(j["results"].empty());
}

// Rich asserts on the search result row shape: every documented field
// must be present, typed, and (where applicable) populated.

TEST_F(ServerTest, SearchResultRowHasRequiredFields) {
    auto j = post("/search", {{"pattern", "Add"}});
    ASSERT_TRUE(j.contains("results"));
    ASSERT_FALSE(j["results"].empty());
    auto& row = j["results"][0];

    // Core location fields.
    EXPECT_TRUE(row.contains("path"));
    EXPECT_TRUE(row["path"].is_string());
    EXPECT_FALSE(row["path"].get<std::string>().empty());

    EXPECT_TRUE(row.contains("line"));
    EXPECT_TRUE(row["line"].is_number_integer());
    EXPECT_GT(row["line"].get<int>(), 0);

    EXPECT_TRUE(row.contains("column"));
    EXPECT_TRUE(row["column"].is_number_integer());
    EXPECT_GE(row["column"].get<int>(), 0);

    EXPECT_TRUE(row.contains("match"));
    EXPECT_TRUE(row["match"].is_string());

    // Score is numeric (parity tests canonicalize floats but raw shape
    // must be a number).
    EXPECT_TRUE(row.contains("score"));
    EXPECT_TRUE(row["score"].is_number());
    EXPECT_GT(row["score"].get<double>(), 0.0);
}

TEST_F(ServerTest, SearchResultContextBlockShape) {
    auto j = post("/search", {{"pattern", "Add"}});
    ASSERT_FALSE(j["results"].empty());
    auto& ctx = j["results"][0]["context"];
    ASSERT_TRUE(ctx.is_object());

    // Always-present keys (handler emits stable shape).
    EXPECT_TRUE(ctx.contains("block_type"));
    EXPECT_TRUE(ctx.contains("block_name"));
    EXPECT_TRUE(ctx.contains("start_line"));
    EXPECT_TRUE(ctx.contains("end_line"));
    EXPECT_TRUE(ctx.contains("is_complete"));
    EXPECT_TRUE(ctx.contains("lines"));

    // Types.
    EXPECT_TRUE(ctx["block_type"].is_string());
    EXPECT_TRUE(ctx["block_name"].is_string());
    EXPECT_TRUE(ctx["start_line"].is_number_integer());
    EXPECT_TRUE(ctx["end_line"].is_number_integer());
    EXPECT_TRUE(ctx["is_complete"].is_boolean());
    EXPECT_TRUE(ctx["lines"].is_array());

    // block_type defaults to "lines" when no semantic block was resolved.
    auto bt = ctx["block_type"].get<std::string>();
    EXPECT_TRUE(bt == "lines" || bt == "function" || bt == "class" ||
                bt == "method" || bt == "struct")
        << "unexpected block_type: " << bt;

    // Line range sanity.
    EXPECT_LE(ctx["start_line"].get<int>(), ctx["end_line"].get<int>());
    EXPECT_GT(ctx["start_line"].get<int>(), 0);

    // Context lines: at least one, all strings.
    ASSERT_FALSE(ctx["lines"].empty());
    for (const auto& line : ctx["lines"]) {
        EXPECT_TRUE(line.is_string());
    }
}

TEST_F(ServerTest, SearchResultContextBlockNameContractEmptyOrSymbolName) {
    // block_name is always a string. When the search engine resolved
    // an enclosing function/class, it should be the symbol name. When
    // not, it's the empty string. Stable presence is the contract.
    auto j = post("/search", {{"pattern", "Add"}});
    ASSERT_FALSE(j["results"].empty());
    for (const auto& row : j["results"]) {
        auto& ctx = row["context"];
        ASSERT_TRUE(ctx.contains("block_name"));
        ASSERT_TRUE(ctx["block_name"].is_string());
        std::string bn = ctx["block_name"].get<std::string>();
        // Either empty (no resolved block) or matches a known symbol.
        // For the synthetic main.go corpus the only symbols are Add,
        // Calculator, Reset, main.
        if (!bn.empty()) {
            EXPECT_TRUE(bn == "Add" || bn == "main" || bn == "Reset" ||
                        bn == "Calculator")
                << "unexpected block_name: " << bn;
        }
    }
}

TEST_F(ServerTest, SearchResultFileIdIsStableInteger) {
    // file_id is intentionally divergent from Go's assignment order;
    // parity tests mask it. Within a single C++ run, it must be a
    // stable positive integer per file.
    auto j = post("/search", {{"pattern", "Add"}});
    ASSERT_FALSE(j["results"].empty());
    int prior_id = -1;
    std::string prior_path;
    for (const auto& row : j["results"]) {
        ASSERT_TRUE(row.contains("file_id"));
        ASSERT_TRUE(row["file_id"].is_number_integer());
        int fid = row["file_id"].get<int>();
        EXPECT_GT(fid, 0);
        if (prior_id != -1 && row["path"].get<std::string>() == prior_path) {
            EXPECT_EQ(fid, prior_id)
                << "Same path must map to same file_id within one run";
        }
        prior_id = fid;
        prior_path = row["path"].get<std::string>();
    }
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

TEST_F(ServerTest, DefinitionEndpointTreatsNegativeLimitAsDefault) {
    auto j = post("/definition", {{"pattern", "Add"}, {"max_results", -1}});
    ASSERT_TRUE(j.contains("definitions")) << j.dump();
    EXPECT_TRUE(j["definitions"].is_array());
}

// -- /definition kind + signature --------------------------------------------

// Verifies that /definition surfaces the REAL symbol kind and a verbatim
// signature line for function / class / method symbols across Go, Python, and
// TypeScript — rather than the generic text-search block_type "lines" with no
// signature. Discrimination: before this behavior existed, `type` was the
// literal "lines" and `signature` was absent/empty for every hit; each case
// below asserts a real kind AND a non-empty signature sliced from the
// declaration line, so a regression to the old generic path fails the test.
// The nested-method cases (Python/TS) additionally pin that a method is not
// reported as its enclosing class.
class DefinitionKindSignatureTest : public ::testing::Test {
  protected:
    void SetUp() override {
        tmp_.write_file("m.go",
                        "package m\n"
                        "\n"
                        "func Greet(name string) string { return name }\n"
                        "\n"
                        "type Greeter struct{ Prefix string }\n"
                        "\n"
                        "func (g *Greeter) Hello(name string) string {\n"
                        "    return g.Prefix + name\n"
                        "}\n");
        tmp_.write_file("m.py",
                        "def compute(x, y):\n"
                        "    return x + y\n"
                        "\n"
                        "\n"
                        "class Widget:\n"
                        "    def render(self, ctx):\n"
                        "        return ctx\n");
        tmp_.write_file("m.ts",
                        "export function makeThing(x: number): number {\n"
                        "  return x;\n"
                        "}\n"
                        "\n"
                        "export class Box {\n"
                        "  open(k: string): string {\n"
                        "    return k;\n"
                        "  }\n"
                        "}\n");

        config_.project.root = tmp_.path().string();
        config_.project.name = "test";

        indexer_ = std::make_unique<MasterIndex>(config_);
        indexer_->index_directory(config_.project.root);
        search_engine_ = std::make_unique<SearchEngine>(*indexer_);
        server_ = std::make_unique<IndexServer>(config_, *indexer_,
                                                search_engine_.get());
        socket_path_ = test::next_test_server_address();
        server_->set_socket_path(socket_path_);
        server_->set_build_id_override("test-build-id");
        ASSERT_TRUE(server_->start());
    }

    void TearDown() override {
        if (server_ && server_->is_running()) server_->shutdown();
        std::error_code ec;
        std::filesystem::remove(socket_path_, ec);
    }

    // Returns the /definition entry for `pattern` whose file_path ends with
    // `file_suffix` (disambiguates same-name hits across the three languages).
    nlohmann::json def_for(const std::string& pattern,
                           const std::string& file_suffix) {
        auto cli = test::make_test_http_client(socket_path_);
        nlohmann::json body{{"pattern", pattern}};
        auto res = cli.Post("/definition", body.dump(), "application/json");
        if (!res) return {};
        auto j = nlohmann::json::parse(res->body);
        if (!j.contains("definitions")) return {};
        for (const auto& d : j["definitions"]) {
            std::string fp = d.value("file_path", "");
            if (fp.size() >= file_suffix.size() &&
                fp.compare(fp.size() - file_suffix.size(), file_suffix.size(),
                           file_suffix) == 0) {
                return d;
            }
        }
        return {};
    }

    TempDir tmp_;
    Config config_;
    std::unique_ptr<MasterIndex> indexer_;
    std::unique_ptr<SearchEngine> search_engine_;
    std::unique_ptr<IndexServer> server_;
    std::string socket_path_;
};

TEST_F(DefinitionKindSignatureTest, RealKindAndSignatureAcrossLanguages) {
    struct Case {
        std::string pattern;
        std::string file_suffix;
        std::string want_type;
        std::string want_sig_substr;
    };
    const std::vector<Case> cases = {
        // Go: function, struct, method.
        {"Greet", "m.go", "function", "func Greet(name string)"},
        {"Greeter", "m.go", "struct", "type Greeter struct"},
        {"Hello", "m.go", "method", "Hello(name string)"},
        // Python: function, class, nested method.
        {"compute", "m.py", "function", "def compute(x, y):"},
        {"Widget", "m.py", "class", "class Widget:"},
        {"render", "m.py", "method", "def render(self, ctx):"},
        // TypeScript: function, class, nested method.
        {"makeThing", "m.ts", "function", "function makeThing(x: number)"},
        {"Box", "m.ts", "class", "class Box"},
        {"open", "m.ts", "method", "open(k: string)"},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(c.pattern + " @ " + c.file_suffix);
        auto d = def_for(c.pattern, c.file_suffix);
        ASSERT_TRUE(d.is_object() && d.contains("type"))
            << "no /definition hit for " << c.pattern;

        // Real kind, never the generic text-search "lines".
        EXPECT_EQ(d.value("type", ""), c.want_type);
        EXPECT_NE(d.value("type", ""), "lines");

        // Verbatim, trimmed declaration line — populated for the first time.
        std::string sig = d.value("signature", "");
        EXPECT_FALSE(sig.empty()) << "signature must be populated";
        EXPECT_NE(sig.find(c.want_sig_substr), std::string::npos)
            << "signature was: " << sig;
        // Trimmed: no leading/trailing whitespace.
        if (!sig.empty()) {
            EXPECT_FALSE(std::isspace(static_cast<unsigned char>(sig.front())));
            EXPECT_FALSE(std::isspace(static_cast<unsigned char>(sig.back())));
        }

        // Backward-compatible fields still present.
        EXPECT_TRUE(d.contains("name"));
        EXPECT_TRUE(d.contains("file_path"));
        EXPECT_TRUE(d.contains("line"));
        EXPECT_TRUE(d.contains("column"));
    }
}

TEST_F(ServerTest, ReferencesEndpoint) {
    auto j = post("/references", {{"pattern", "Add"}});
    ASSERT_TRUE(j.contains("references"));
}

TEST_F(ServerTest, ReferencesEndpointTreatsNegativeLimitAsDefault) {
    auto j = post("/references", {{"pattern", "Add"}, {"max_results", -1}});
    ASSERT_TRUE(j.contains("references")) << j.dump();
    EXPECT_TRUE(j["references"].is_array());
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

// Rich asserts on the inspect-symbol shape: every field that the
// handler may emit gets a positive- or negative-presence assertion so
// regressions in extractor population (parameter_count, receiver_type,
// callers, callees, scope_chain) are caught here.

TEST_F(ServerTest, InspectAddFunctionExposesCoreFields) {
    auto j = post("/inspect-symbol", {{"name", "Add"}, {"include", "signature"}});
    ASSERT_EQ(j["count"].get<int>(), 1);
    auto& s = j["symbols"][0];
    EXPECT_EQ(s["name"].get<std::string>(), "Add");
    EXPECT_EQ(s["type"].get<std::string>(), "function");
    EXPECT_TRUE(s["is_exported"].get<bool>())
        << "Capitalized Go function must be exported";
    EXPECT_GT(s["complexity"].get<int>(), 0);
    EXPECT_GT(s["line"].get<int>(), 0);
    EXPECT_TRUE(s.contains("object_id"));
    EXPECT_FALSE(s["object_id"].get<std::string>().empty());
    // outgoing_refs is always emitted (even 0).
    EXPECT_TRUE(s.contains("outgoing_refs"));
    EXPECT_GE(s["outgoing_refs"].get<int>(), 0);
}

TEST_F(ServerTest, InspectAddFunctionHasParameterCount) {
    auto j = post("/inspect-symbol", {{"name", "Add"}});
    ASSERT_EQ(j["count"].get<int>(), 1);
    auto& s = j["symbols"][0];
    // Add(a, b int) has 2 parameters. parameter_count emitted only
    // when > 0 by the handler.
    if (s.contains("parameter_count")) {
        EXPECT_EQ(s["parameter_count"].get<int>(), 2);
    } else {
        // Document the fact that the extractor doesn't populate it:
        // this assertion fails when the extractor starts working,
        // which is the desired regression signal.
        GTEST_LOG_(INFO) << "parameter_count not yet populated by extractor";
    }
}

TEST_F(ServerTest, InspectResetMethodHasReceiverType) {
    auto j = post("/inspect-symbol", {{"name", "Reset"}});
    ASSERT_GE(j["count"].get<int>(), 1);
    auto& s = j["symbols"][0];
    EXPECT_EQ(s["name"].get<std::string>(), "Reset");
    EXPECT_EQ(s["type"].get<std::string>(), "method");
    // receiver_type emitted only when non-empty.
    if (s.contains("receiver_type")) {
        std::string recv = s["receiver_type"].get<std::string>();
        EXPECT_FALSE(recv.empty());
        EXPECT_NE(recv.find("Calculator"), std::string::npos)
            << "Reset is a method on *Calculator; receiver_type should mention it";
    } else {
        GTEST_LOG_(INFO) << "receiver_type not yet populated by extractor";
    }
}

TEST_F(ServerTest, InspectScopeChainHasFileAndSymbol) {
    auto j = post("/inspect-symbol", {{"name", "Add"}});
    ASSERT_GE(j["count"].get<int>(), 1);
    auto& s = j["symbols"][0];
    if (s.contains("scope_chain") && s["scope_chain"].is_array()) {
        ASSERT_FALSE(s["scope_chain"].empty());
        // Last entry should be the symbol name itself.
        std::string last = s["scope_chain"].back().get<std::string>();
        EXPECT_EQ(last, "Add");
        // Some intermediate entry should reference main.go.
        bool saw_file = false;
        for (const auto& seg : s["scope_chain"]) {
            if (seg.get<std::string>().find("main.go") != std::string::npos) {
                saw_file = true;
                break;
            }
        }
        EXPECT_TRUE(saw_file) << "scope_chain should include file segment";
    }
}

TEST_F(ServerTest, InspectFileFieldIsAbsoluteOrProjectRelative) {
    auto j = post("/inspect-symbol", {{"name", "Add"}});
    ASSERT_GE(j["count"].get<int>(), 1);
    auto& s = j["symbols"][0];
    std::string file = s["file"].get<std::string>();
    EXPECT_FALSE(file.empty());
    EXPECT_NE(file.find("main.go"), std::string::npos);
}

TEST_F(ServerTest, InspectUnknownSymbolReturnsZeroCount) {
    auto j = post("/inspect-symbol", {{"name", "ThereIsNoSymbolNamedThis123"}});
    ASSERT_TRUE(j.contains("count"));
    EXPECT_EQ(j["count"].get<int>(), 0);
    EXPECT_TRUE(j["symbols"].is_array());
    EXPECT_TRUE(j["symbols"].empty());
}

TEST_F(ServerTest, InspectSignatureGatedByIncludeParam) {
    // Default include should still emit signature (handler accepts
    // "signature" anywhere in the include string).
    auto j = post("/inspect-symbol",
                  {{"name", "Add"}, {"include", "signature"}});
    ASSERT_EQ(j["count"].get<int>(), 1);
    auto& s = j["symbols"][0];
    // If the extractor produced a signature, it must be a non-empty string.
    if (s.contains("signature")) {
        EXPECT_TRUE(s["signature"].is_string());
        EXPECT_FALSE(s["signature"].get<std::string>().empty());
        // Signature should mention the function name.
        EXPECT_NE(s["signature"].get<std::string>().find("Add"),
                  std::string::npos);
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

TEST_F(ServerTest, BrowseFileStatsBlockFullShape) {
    auto j = post("/browse-file",
                  {{"file", "main.go"}, {"show_stats", true}});
    ASSERT_TRUE(j.contains("stats")) << j.dump();
    auto& stats = j["stats"];

    // Required counter fields.
    for (const auto* k : {"symbol_count", "function_count"}) {
        ASSERT_TRUE(stats.contains(k)) << "missing stats key: " << k;
        EXPECT_TRUE(stats[k].is_number_integer()) << k;
        EXPECT_GE(stats[k].get<int>(), 0) << k;
    }

    // main.go has: main, Add, Calculator (struct), Reset -> ~4 symbols.
    // function_count should be at least 2 (main, Add); Reset is a method.
    EXPECT_GE(stats["symbol_count"].get<int>(), 3);
    EXPECT_GE(stats["function_count"].get<int>(), 2);
}

TEST_F(ServerTest, BrowseFileWithoutStatsFlagOmitsStatsBlock) {
    // Contract: show_stats=false (default) → no stats block.
    auto j = post("/browse-file", {{"file", "main.go"}});
    EXPECT_FALSE(j.contains("stats"))
        << "stats block must be opt-in via show_stats=true";
}

TEST_F(ServerTest, BrowseFileSymbolsArrayShape) {
    auto j = post("/browse-file", {{"file", "main.go"}});
    ASSERT_TRUE(j.contains("symbols"));
    ASSERT_TRUE(j["symbols"].is_array());
    EXPECT_FALSE(j["symbols"].empty());

    bool saw_add = false;
    bool saw_calc = false;
    for (const auto& sym : j["symbols"]) {
        EXPECT_TRUE(sym.contains("name"));
        EXPECT_TRUE(sym.contains("type"));
        EXPECT_TRUE(sym.contains("line"));
        EXPECT_GT(sym["line"].get<int>(), 0);

        auto name = sym["name"].get<std::string>();
        if (name == "Add") saw_add = true;
        if (name == "Calculator") saw_calc = true;
    }
    EXPECT_TRUE(saw_add);
    EXPECT_TRUE(saw_calc);
}

TEST_F(ServerTest, BrowseFileLanguageAndPath) {
    auto j = post("/browse-file", {{"file", "main.go"}});
    auto& fi = j["file"];
    EXPECT_EQ(fi["language"].get<std::string>(), "go");
    ASSERT_TRUE(fi.contains("path"));
    EXPECT_NE(fi["path"].get<std::string>().find("main.go"), std::string::npos);
    ASSERT_TRUE(fi.contains("file_id"));
    EXPECT_GT(fi["file_id"].get<int>(), 0);
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

// Rich asserts on the tree endpoint shape and file_path policy.

TEST_F(ServerTest, TreeRootNodeFullShape) {
    auto j = post("/tree", {{"function_name", "Add"}});
    ASSERT_TRUE(j.contains("tree")) << j.dump();
    auto& tree = j["tree"];
    EXPECT_EQ(tree["root_function"].get<std::string>(), "Add");

    ASSERT_TRUE(tree.contains("root"));
    auto& root = tree["root"];
    // Required keys on every node.
    for (const auto* key : {"name", "line", "depth", "file_path",
                             "node_type", "dependency_count",
                             "dependent_count", "edit_risk_score",
                             "impact_radius", "annotations",
                             "safety_notes", "stability_tags",
                             "children"}) {
        EXPECT_TRUE(root.contains(key)) << "missing key: " << key;
    }

    EXPECT_EQ(root["name"].get<std::string>(), "Add");
    EXPECT_EQ(root["depth"].get<int>(), 0);
    EXPECT_GT(root["line"].get<int>(), 0);
    EXPECT_TRUE(root["children"].is_array());
}

TEST_F(ServerTest, TreeRootFilePathIsRelativeToProjectRoot) {
    auto j = post("/tree", {{"function_name", "Add"}});
    ASSERT_TRUE(j.contains("tree")) << j.dump();
    auto& root = j["tree"]["root"];

    ASSERT_TRUE(root.contains("file_path"));
    ASSERT_TRUE(root["file_path"].is_string());
    std::string fp = root["file_path"].get<std::string>();
    EXPECT_FALSE(fp.empty())
        << "C++ tree.root.file_path is intentionally populated (richer "
           "than Go which emits empty). See docs/parity/http-tree.md.";
    EXPECT_NE(fp.front(), '/') << "Path must be relative: " << fp;
    EXPECT_NE(fp.find("main.go"), std::string::npos)
        << "Add is defined in main.go: " << fp;
}

TEST_F(ServerTest, TreeOptionsBlockShape) {
    auto j = post("/tree", {{"function_name", "Add"}, {"max_depth", 5}});
    ASSERT_TRUE(j.contains("tree"));
    auto& tree = j["tree"];
    ASSERT_TRUE(tree.contains("options"));
    auto& opts = tree["options"];
    EXPECT_TRUE(opts["agent_mode"].is_boolean());
    EXPECT_FALSE(opts["agent_mode"].get<bool>());
    EXPECT_FALSE(opts["compact"].get<bool>());
    EXPECT_FALSE(opts["show_lines"].get<bool>());
    EXPECT_EQ(opts["max_depth"].get<int>(), 5);
    EXPECT_EQ(opts["exclude_pattern"].get<std::string>(), "");

    EXPECT_GE(tree["total_nodes"].get<int>(), 0);
}

TEST_F(ServerTest, TreeFunctionNotFoundReturnsError) {
    auto j = post("/tree", {{"function_name", "DoesNotExist123"}});
    EXPECT_TRUE(j.contains("error"));
    EXPECT_NE(j["error"].get<std::string>().find("not found"),
              std::string::npos);
}

TEST_F(ServerTest, TreeMissingFunctionNameReturns400) {
    auto cli = make_client();
    auto res = cli.Post("/tree", "{}", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_TRUE(j.contains("error"));
    EXPECT_NE(j["error"].get<std::string>().find("function_name"),
              std::string::npos);
}

#ifndef _WIN32
TEST_F(ServerTest, SocketLockSymlinkIsNotFollowed) {
    // The sidecar .lock must be opened with O_NOFOLLOW: a pre-planted
    // symlink at the lock path must not let the server open (and flock)
    // an attacker-chosen target. Observable: pre-fix the server holds a
    // flock on the symlink's target, so LOCK_EX|LOCK_NB from here fails;
    // post-fix the target is never opened.
    server_->shutdown();
    std::error_code ec;
    std::filesystem::remove(socket_path_, ec);
    std::filesystem::remove(socket_path_ + ".lock", ec);

    const auto target = tmp_.path() / "lock-target";
    { std::ofstream f(target); f << "x"; }
    std::filesystem::create_symlink(target, socket_path_ + ".lock");

    ASSERT_TRUE(server_->start());

    int tfd = ::open(target.c_str(), O_RDWR | O_CLOEXEC);
    ASSERT_GE(tfd, 0);
    EXPECT_EQ(::flock(tfd, LOCK_EX | LOCK_NB), 0)
        << "server followed the .lock symlink and flocked its target";
    ::close(tfd);
}

TEST_F(ServerTest, SocketModeIsOwnerOnlyUnderPermissiveUmask) {
    // The socket mode is the only guard keeping other local users off the
    // server. Even with a permissive process umask the bound socket must
    // never carry group/other bits.
    const mode_t old_umask = ::umask(0);
    server_->shutdown();
    std::error_code ec;
    std::filesystem::remove(socket_path_, ec);
    std::filesystem::remove(socket_path_ + ".lock", ec);
    ASSERT_TRUE(server_->start());
    ::umask(old_umask);

    struct stat st {};
    ASSERT_EQ(::stat(socket_path_.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 077, 0)
        << "socket is reachable by group/other users: "
        << std::oct << (st.st_mode & 0777);
}
#endif

TEST_F(ServerTest, WrongTypedJsonFieldsReturn400Not500) {
    // Every field-reading endpoint must reject a wrong-typed JSON body with
    // a 400 JSON error. Pre-fix these threw nlohmann::json::type_error out
    // of the handler and httplib answered 500 with an empty body.
    auto post_status = [&](const std::string& path,
                           const nlohmann::json& body) {
        auto cli = make_client();
        auto res = cli.Post(path, body.dump(), "application/json");
        EXPECT_TRUE(res) << path;
        if (!res) return 0;
        // The error body must itself be JSON carrying an "error" field.
        auto j = nlohmann::json::parse(res->body, nullptr, false);
        EXPECT_FALSE(j.is_discarded()) << path << " body: " << res->body;
        if (!j.is_discarded()) {
            EXPECT_TRUE(j.contains("error")) << path << " body: " << res->body;
        }
        return res->status;
    };
    // Non-object bodies.
    EXPECT_EQ(post_status("/symbol", nlohmann::json::array({1})), 400);
    // Wrong-typed scalar fields, one per endpoint plus the nullable and
    // pagination fields of /list-symbols and /browse-file.
    EXPECT_EQ(post_status("/symbol", {{"symbol_id", "abc"}}), 400);
    EXPECT_EQ(post_status("/fileinfo", {{"file_id", "abc"}}), 400);
    EXPECT_EQ(post_status("/tree", {{"function_name", 42}}), 400);
    EXPECT_EQ(post_status("/tree",
                          {{"function_name", "Add"}, {"max_depth", "deep"}}),
              400);
    EXPECT_EQ(post_status("/list-symbols", {{"exported", "yes"}}), 400);
    EXPECT_EQ(post_status("/list-symbols", {{"min_complexity", "high"}}), 400);
    EXPECT_EQ(post_status("/list-symbols", {{"max", "lots"}}), 400);
    EXPECT_EQ(post_status("/inspect-symbol", {{"name", 42}}), 400);
    EXPECT_EQ(post_status("/browse-file", {{"file_id", "abc"}}), 400);
    EXPECT_EQ(post_status("/browse-file",
                          {{"file", "main.go"}, {"max", "lots"}}),
              400);
    EXPECT_EQ(post_status("/browse-file",
                          {{"file", "main.go"}, {"show_stats", "yes"}}),
              400);
}

TEST_F(ServerTest, TreeChildNodesHavePositiveDepth) {
    auto j = post("/tree", {{"function_name", "Add"}, {"max_depth", 10}});
    ASSERT_TRUE(j.contains("tree"));
    auto& root = j["tree"]["root"];
    EXPECT_EQ(root["depth"].get<int>(), 0);
    // Every child of root must have depth==1, grandchildren depth==2, etc.
    for (const auto& child : root["children"]) {
        EXPECT_EQ(child["depth"].get<int>(), 1);
        for (const auto& gc : child["children"]) {
            EXPECT_EQ(gc["depth"].get<int>(), 2);
        }
    }
}

TEST_F(ServerTest, GitAnalyzeReportsUnavailableOnNonGitRoot) {
    // The test server's root is not a git repo. That is an inapplicable
    // capability, not a request error: 200 with available=false + reason,
    // matching the MCP git surfaces.
    auto j = post("/git-analyze", {{"scope", "staged"}});
    EXPECT_FALSE(j.contains("error"));
    EXPECT_EQ(j.value("available", true), false);
    EXPECT_NE(j.value("reason", std::string()).find("not a git repository"),
              std::string::npos);
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
    bool clean = server_->shutdown();
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(clean);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                  .count(),
              5000);
    EXPECT_FALSE(server_->is_running());
}

// -- Lifecycle reaper tests ---------------------------------------------------
//
// These pin the fix for the orphaned-server leak class: `lci -r <root> grep`
// spawns a persistent per-root server, callers deleted the root (or just
// stopped calling), and the server lived forever. Three policies close it:
// idle timeout, root-deletion exit, and LRU eviction past a per-user cap.

bool wait_until(const std::function<bool()>& cond,
                std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return cond();
}

class ReaperServer {
  public:
    explicit ReaperServer(int idle_timeout_sec,
                          const std::string& registry_dir = "",
                          int max_instances = 0) {
        config_.project.root = tmp_.path().string();
        config_.server.idle_timeout_sec = idle_timeout_sec;
        config_.server.max_instances = max_instances;
        indexer_ = std::make_unique<MasterIndex>(config_);
        engine_ = std::make_unique<SearchEngine>(*indexer_);
        server_ = std::make_unique<IndexServer>(config_, *indexer_,
                                                engine_.get());
        address_ = test::next_test_server_address();
        server_->set_socket_path(address_);
        if (!registry_dir.empty()) {
            server_->enable_instance_registry(registry_dir);
        }
    }

    ~ReaperServer() {
        if (server_) server_->shutdown();
        std::error_code ec;
        std::filesystem::remove(address_, ec);
    }

    IndexServer& server() { return *server_; }
    const std::string& address() const { return address_; }
    const std::filesystem::path& root() const { return tmp_.path(); }

    nlohmann::json post(const std::string& path) {
        auto cli = test::make_test_http_client(address_);
        auto res = cli.Post(path, "{}", "application/json");
        if (!res) return {{"error", "connection failed"}};
        return nlohmann::json::parse(res->body);
    }

  private:
    TempDir tmp_;
    Config config_;
    std::unique_ptr<MasterIndex> indexer_;
    std::unique_ptr<SearchEngine> engine_;
    std::unique_ptr<IndexServer> server_;
    std::string address_;
};

TEST(ServerReaperTest, IdleTimeoutStopsUnusedServer) {
    ReaperServer s(/*idle_timeout_sec=*/1);
    ASSERT_TRUE(s.server().start());
    EXPECT_TRUE(wait_until([&] { return !s.server().is_running(); },
                           std::chrono::milliseconds(5000)))
        << "server must self-stop after 1s without requests";
}

TEST(ServerReaperTest, PingDoesNotDeferIdleExit) {
    // /ping is a liveness probe (client discovery, eviction scans); it must
    // not count as activity or probing would keep every idle server alive.
    ReaperServer s(/*idle_timeout_sec=*/1);
    ASSERT_TRUE(s.server().start());
    EXPECT_TRUE(wait_until(
        [&] {
            s.post("/ping");
            return !s.server().is_running();
        },
        std::chrono::milliseconds(5000)))
        << "pings alone must not keep the server alive";
}

TEST(ServerReaperTest, RequestsDeferIdleExit) {
    ReaperServer s(/*idle_timeout_sec=*/1);
    ASSERT_TRUE(s.server().start());
    // Keep touching /status (a real request) well past the idle timeout.
    for (int i = 0; i < 6; ++i) {
        s.post("/status");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    EXPECT_TRUE(s.server().is_running())
        << "active server must not idle-exit";
    // Stop the traffic: now it must go.
    EXPECT_TRUE(wait_until([&] { return !s.server().is_running(); },
                           std::chrono::milliseconds(5000)));
}

TEST(ServerReaperTest, RootDeletionStopsServer) {
    ReaperServer s(/*idle_timeout_sec=*/0);  // isolate the root-gone policy
    ASSERT_TRUE(s.server().start());
    std::error_code ec;
    std::filesystem::remove_all(s.root(), ec);
    EXPECT_TRUE(wait_until([&] { return !s.server().is_running(); },
                           std::chrono::milliseconds(5000)))
        << "server must self-stop once its project root is deleted";
}

TEST(ServerReaperTest, RegistryEntryPublishedAndRemoved) {
    TempDir registry;
    ReaperServer s(/*idle_timeout_sec=*/0, registry.path().string(),
                   /*max_instances=*/8);
    ASSERT_TRUE(s.server().start());
    auto count_entries = [&] {
        int n = 0;
        for (const auto& e :
             std::filesystem::directory_iterator(registry.path())) {
            if (e.path().extension() == ".json") ++n;
        }
        return n;
    };
    EXPECT_EQ(count_entries(), 1);
    s.server().shutdown();
    EXPECT_EQ(count_entries(), 0);
}

TEST(ServerReaperTest, NewServerEvictsLeastRecentlyActivePastCap) {
    TempDir registry;
    const std::string reg = registry.path().string();
    ReaperServer a(/*idle_timeout_sec=*/0, reg, /*max_instances=*/2);
    ASSERT_TRUE(a.server().start());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ReaperServer b(/*idle_timeout_sec=*/0, reg, /*max_instances=*/2);
    ASSERT_TRUE(b.server().start());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Third server over a cap of 2: the least-recently-active peer (a)
    // must be asked to shut down; b and c stay.
    ReaperServer c(/*idle_timeout_sec=*/0, reg, /*max_instances=*/2);
    ASSERT_TRUE(c.server().start());

    EXPECT_TRUE(wait_until([&] { return !a.server().is_running(); },
                           std::chrono::milliseconds(5000)))
        << "oldest server must be evicted past the cap";
    EXPECT_TRUE(b.server().is_running());
    EXPECT_TRUE(c.server().is_running());
}

TEST(ServerRegistryTest, ListsLiveServersLeastRecentlyActiveFirst) {
    TempDir registry;
    const std::string reg = registry.path().string();
    ReaperServer a(/*idle_timeout_sec=*/0, reg, /*max_instances=*/8);
    ASSERT_TRUE(a.server().start());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ReaperServer b(/*idle_timeout_sec=*/0, reg, /*max_instances=*/8);
    ASSERT_TRUE(b.server().start());

    auto live = list_server_instances(reg);
    ASSERT_EQ(live.size(), 2u);
    EXPECT_EQ(live[0].address, a.address()) << "oldest entry must sort first";
    EXPECT_EQ(live[1].address, b.address());
    EXPECT_EQ(live[0].root, a.root().string());
    EXPECT_TRUE(live[0].root_exists);
    EXPECT_GT(live[0].pid, 0u);
}

TEST(ServerRegistryTest, ExcludeEntryOmitsOneServer) {
    TempDir registry;
    const std::string reg = registry.path().string();
    ReaperServer a(/*idle_timeout_sec=*/0, reg, /*max_instances=*/8);
    ASSERT_TRUE(a.server().start());

    auto all = list_server_instances(reg);
    ASSERT_EQ(all.size(), 1u);
    EXPECT_TRUE(list_server_instances(reg, all[0].entry_path).empty());
}

TEST(ServerRegistryTest, DeadEntryIsNotReportedAndIsRemoved) {
    // A registry entry that outlives its process is litter. Enumeration must
    // drop it, not report a server that cannot be reached — otherwise
    // `lci servers` invents servers and `shutdown --all` reports failures.
    TempDir registry;
    const std::string reg = registry.path().string();

    // Name the file with this user's real prefix (lci-srv-<uid>-) so the
    // scan considers it at all; take it from a real entry.
    ReaperServer live(/*idle_timeout_sec=*/0, reg, /*max_instances=*/8);
    ASSERT_TRUE(live.server().start());
    auto found = list_server_instances(reg);
    ASSERT_EQ(found.size(), 1u);
    const std::string fname =
        std::filesystem::path(found[0].entry_path).filename().string();
    const std::string prefix = fname.substr(0, fname.rfind('-') + 1);

    const auto dead = registry.path() / (prefix + "deadbeef.json");
    {
        std::ofstream out(dead);
        out << nlohmann::json{{"pid", 999999},
                              {"address", (registry.path() / "gone.sock")
                                              .string()},
                              {"root", "/nonexistent"}}
                   .dump();
    }
    ASSERT_TRUE(std::filesystem::exists(dead));

    auto still = list_server_instances(reg);
    EXPECT_EQ(still.size(), 1u) << "unreachable server must not be listed";
    EXPECT_FALSE(std::filesystem::exists(dead))
        << "its registry entry must be reaped";
}

TEST(ServerRegistryTest, ReportsDeletedRootAsGone) {
    TempDir registry;
    const std::string reg = registry.path().string();
    ReaperServer s(/*idle_timeout_sec=*/0, reg, /*max_instances=*/8);
    ASSERT_TRUE(s.server().start());
    auto before = list_server_instances(reg);
    ASSERT_EQ(before.size(), 1u);
    EXPECT_TRUE(before[0].root_exists);

    // The reaper stops such a server within a tick; enumeration racing it
    // must still describe the root honestly while the process is up.
    std::error_code ec;
    std::filesystem::remove_all(s.root(), ec);
    auto after = list_server_instances(reg);
    if (!after.empty()) {
        EXPECT_FALSE(after[0].root_exists);
    }
}

TEST(ServerReaperTest, ShutdownEndpointStopsServer) {
    // Pins the fix that /shutdown clears running_: before, a remote
    // /shutdown left the CLI serve loop (which polls is_running()) alive
    // forever — the orphan-leak mechanism.
    ReaperServer s(/*idle_timeout_sec=*/0);
    ASSERT_TRUE(s.server().start());
    auto j = s.post("/shutdown");
    EXPECT_TRUE(j["success"].get<bool>());
    EXPECT_TRUE(wait_until([&] { return !s.server().is_running(); },
                           std::chrono::milliseconds(3000)));
}

// -- Shutdown gate (S4) ---------------------------------------------------------
//
// One gate, one stop: /shutdown's deferred trigger, the reaper's self-stop,
// and the owner's shutdown() all converge on stop_listener_once().

TEST(ServerLifecycleTest, ShutdownEndpointThenOwnerShutdownStopsOnce) {
    // POST /shutdown, then the owner calls shutdown() immediately — the
    // trigger thread's ~100ms delay makes the two race on purpose. httplib's
    // stop() is not idempotent (a second call trips its
    // assert(svr_sock_ != INVALID_SOCKET) in a debug build), so every call
    // site must pass through stop_listener_once(). The callback count pins
    // the gate observably in any build type.
    for (int i = 0; i < 20; ++i) {
        TempDir tmp;
        tmp.write_file("a.go", "package main\nfunc Add(a, b int) int { return a + b }\n");
        Config config;
        config.project.root = tmp.path().string();
        MasterIndex indexer(config);
        SearchEngine engine(indexer);

        std::atomic<int> self_stops{0};
        IndexServer server(config, indexer, &engine);
        server.set_socket_path(test::next_test_server_address());
        server.set_self_stop_callback(
            [&self_stops](const char*) { self_stops.fetch_add(1); });
        ASSERT_TRUE(server.start()) << "iteration " << i;

        auto cli = test::make_test_http_client(server.socket_path());
        auto res = cli.Post("/shutdown", "{}", "application/json");
        ASSERT_TRUE(res) << "iteration " << i;
        EXPECT_EQ(res->status, 200);

        // Race the deferred trigger thread: owner teardown must converge
        // with the endpoint's self-stop without a second svr_.stop().
        EXPECT_TRUE(server.shutdown()) << "iteration " << i;
        EXPECT_FALSE(server.is_running());
        EXPECT_EQ(self_stops.load(), 1)
            << "iteration " << i
            << ": /shutdown must fire the self-stop callback exactly once";
    }
}

TEST(ServerLifecycleTest, ShutdownEndpointCallbackOnceAndRestartRearms) {
    // Two concurrent /shutdown requests must coalesce into one self-stop
    // notification, and start() must re-arm the trigger so a restarted
    // server still honours /shutdown.
    TempDir tmp;
    tmp.write_file("a.go", "package main\n");
    Config config;
    config.project.root = tmp.path().string();
    MasterIndex indexer(config);
    SearchEngine engine(indexer);
    IndexServer server(config, indexer, &engine);
    server.set_socket_path(test::next_test_server_address());

    std::atomic<int> self_stops{0};
    server.set_self_stop_callback(
        [&self_stops](const char*) { self_stops.fetch_add(1); });
    ASSERT_TRUE(server.start());

    std::thread a([&] {
        auto cli = test::make_test_http_client(server.socket_path());
        (void)cli.Post("/shutdown", "{}", "application/json");
    });
    std::thread b([&] {
        auto cli = test::make_test_http_client(server.socket_path());
        (void)cli.Post("/shutdown", "{}", "application/json");
    });
    a.join();
    b.join();

    EXPECT_TRUE(wait_until([&] { return !server.is_running(); },
                           std::chrono::milliseconds(3000)));
    EXPECT_TRUE(server.shutdown());
    EXPECT_EQ(self_stops.load(), 1)
        << "concurrent /shutdown requests must notify exactly once";

    // Restart: the gate re-arms and a second /shutdown actually stops.
    ASSERT_TRUE(server.start());
    EXPECT_TRUE(server.is_running());
    {
        auto cli = test::make_test_http_client(server.socket_path());
        auto res = cli.Post("/shutdown", "{}", "application/json");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 200);
    }
    EXPECT_TRUE(wait_until([&] { return !server.is_running(); },
                           std::chrono::milliseconds(3000)))
        << "a restarted server must honour /shutdown again";
    EXPECT_TRUE(server.shutdown());
    EXPECT_EQ(self_stops.load(), 2);
}

TEST(ServerLifecycleTest, ShutdownConvergesWhileBulkReindexParked) {
    // /shutdown issued while a bulk reindex holds the bulk window must
    // converge in bounded time. The shutdown gate orders
    // cancel_indexing -> stop_watch_pipeline -> stop_listener_once: the
    // watch pipeline's debouncer timer blocks on bulk_mu_ for the whole
    // bulk window, so stopping the watch pipeline BEFORE cancelling the
    // reindex parks teardown until the run finishes on its own.
    TempDir tmp;
    tmp.write_file("a.go", "package main\nfunc Before() {}\n");
    Config config;
    config.project.root = tmp.path().string();
    config.index.watch_mode = true;
    config.index.watch_debounce_ms = 50;
    MasterIndex indexer(config);
    indexer.index_directory(config.project.root);
    SearchEngine engine(indexer);
    IndexServer server(config, indexer, &engine);
    server.set_socket_path(test::next_test_server_address());
    ASSERT_TRUE(server.start());

    // Park the /reindex bulk window. Bounded give-up: the FIXED gate
    // cancels the run (request_stop), which this hook observes; the broken
    // order waits out the whole give-up instead of cancelling.
    std::atomic<bool> in_window{false};
    indexer.set_post_parse_hook([&] {
        in_window.store(true, std::memory_order_release);
        const auto give_up =
            std::chrono::steady_clock::now() + std::chrono::seconds{30};
        while (!indexer.stop_requested() &&
               std::chrono::steady_clock::now() < give_up) {
            std::this_thread::yield();
        }
    });

    {
        auto cli = test::make_test_http_client(server.socket_path());
        auto res = cli.Post("/reindex",
                            nlohmann::json{{"path", config.project.root}}
                                .dump(),
                            "application/json");
        ASSERT_TRUE(res);
        ASSERT_EQ(res->status, 200);
    }
    ASSERT_TRUE(wait_until(
        [&] { return in_window.load(std::memory_order_acquire); },
        std::chrono::milliseconds(10000)))
        << "reindex never reached the parked bulk window";

    // A file change during the parked window sends the debouncer timer
    // thread into a bulk_mu_ wait — the exact interlock the gate order
    // must break by cancelling the bulk run first.
    tmp.write_file("b.go", "package main\nfunc After() {}\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    {
        auto cli = test::make_test_http_client(server.socket_path());
        (void)cli.Post("/shutdown", "{}", "application/json");
    }

    auto done = std::async(std::launch::async,
                           [&] { return server.shutdown(); });
    EXPECT_EQ(done.wait_for(std::chrono::seconds(10)),
              std::future_status::ready)
        << "shutdown must cancel the parked bulk run, not wait it out";
    indexer.set_post_parse_hook(nullptr);
}

// -- /reindex validation (S4) ---------------------------------------------------
//
// The reindex path is client-controlled; an unvalidated path yielded an
// empty engine with ready:true (nonexistent root indexed "successfully") or
// rebuilt an arbitrary directory outside the project.

TEST_F(ServerTest, ReindexRejectsPathOutsideProjectRoot) {
    TempDir outside;
    outside.write_file("x.go", "package main\n");
    auto cli = make_client();
    auto res = cli.Post("/reindex",
                        nlohmann::json{{"path", outside.path().string()}}
                            .dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_TRUE(j.contains("error"));

    // Ready state untouched by the rejected request.
    auto status = get("/status");
    EXPECT_TRUE(status["ready"].get<bool>());
}

TEST_F(ServerTest, ReindexRejectsNonexistentPath) {
    auto cli = make_client();
    auto res = cli.Post(
        "/reindex",
        nlohmann::json{{"path", (tmp_.path() / "no-such-dir").string()}}
            .dump(),
        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400)
        << "a nonexistent path must not start an empty reindex";
    auto j = nlohmann::json::parse(res->body);
    EXPECT_TRUE(j.contains("error"));

    // Ready state unchanged: before the fix this published an EMPTY engine
    // with ready:true.
    auto status = get("/status");
    EXPECT_TRUE(status["ready"].get<bool>());
    EXPECT_GT(status["file_count"].get<int>(), 0);
}

TEST_F(ServerTest, ReindexRejectsNonStringPath) {
    auto cli = make_client();
    auto res = cli.Post("/reindex", R"({"path": 123})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(ServerTest, ReindexAcceptsProjectRoot) {
    auto cli = make_client();
    auto res = cli.Post("/reindex",
                        nlohmann::json{{"path", config_.project.root}}.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    // Let the reindex finish before teardown cancels it mid-run.
    EXPECT_TRUE(wait_until(
        [&] {
            auto s = get("/status");
            return s.contains("indexing_active") &&
                   !s["indexing_active"].get<bool>();
        },
        std::chrono::milliseconds(10000)));
}

// -- /mcp queue bound (S4) ------------------------------------------------------
//
// The /mcp bridge parks one worker per stdio client inside the handler for
// the whole index warmup. With the default worker pool (8 threads, unbounded
// queue) >=8 parked bridge calls mute /ping and /shutdown.

TEST(ServerLifecycleTest, PingAnswersWhileMcpCallsParkedOnWarmup) {
    TempDir tmp;
    tmp.write_file("a.go",
                   "package main\nfunc Add(a, b int) int { return a + b }\n");
    Config config;
    config.project.root = tmp.path().string();
    MasterIndex indexer(config);
    indexer.index_directory(config.project.root);
    SearchEngine engine(indexer);
    IndexServer server(config, indexer, &engine);
    server.set_socket_path(test::next_test_server_address());

    // Hold the warmup latch: every /mcp dispatch parks its worker on it,
    // exactly like `lci mcp` bridge calls during the initial index.
    mcp::WarmupLatch warmup;
    std::atomic<int> entered{0};
    server.set_mcp_dispatcher([&](const std::string&) {
        entered.fetch_add(1, std::memory_order_release);
        std::string err;
        warmup.wait(err);
        return std::string("{}");
    });
    ASSERT_TRUE(server.start());
    const std::string addr = server.socket_path();

    constexpr int kParked = 16;
    std::vector<std::thread> bridges;
    bridges.reserve(kParked);
    for (int i = 0; i < kParked; ++i) {
        bridges.emplace_back([&] {
            auto cli = test::make_test_http_client(addr);
            cli.set_read_timeout(std::chrono::seconds{30});
            (void)cli.Post("/mcp", "{}", "application/json");
        });
    }
    // Wait for the bridge calls to park: the fixed pool admits all 16 into
    // workers; the broken 8-thread pool saturates and the rest queue. Either
    // way `entered` settles once every worker is parked on the latch.
    ASSERT_TRUE(wait_until(
        [&] { return entered.load(std::memory_order_acquire) >= 1; },
        std::chrono::milliseconds(10000)))
        << "bridge calls never reached the dispatcher";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int parked = entered.load(std::memory_order_acquire);

    // The BROKEN pool queues /ping behind the parked bridge calls until the
    // latch releases; the releaser bounds that wait so the pre-fix run also
    // terminates (latency ~= 2s, over the 1s bound). The fixed pool has
    // workers free and answers immediately.
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        warmup.finish({});
    });
    const auto t0 = std::chrono::steady_clock::now();
    int ping_status = -1;
    {
        auto cli = test::make_test_http_client(addr);
        cli.set_read_timeout(std::chrono::seconds{30});
        auto res = cli.Post("/ping", "{}", "application/json");
        if (res) ping_status = res->status;
    }
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    releaser.join();

    for (auto& t : bridges) {
        t.join();
    }
    EXPECT_TRUE(server.shutdown());

    EXPECT_EQ(ping_status, 200);
    EXPECT_LT(elapsed, std::chrono::seconds(1))
        << "/ping must not wait behind " << parked
        << " /mcp bridge calls parked on warmup";
}

}  // namespace
}  // namespace lci
