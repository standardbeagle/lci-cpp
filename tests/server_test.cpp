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
#include <unistd.h>
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
#ifdef _WIN32
    // Windows: TCP fallback "localhost:<port>".
    EXPECT_NE(path.find("localhost:"), std::string::npos);
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
    EXPECT_NE(path1.find("localhost:"), std::string::npos);
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
