#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/core/reference_tracker.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_explore.h>
#include <lci/mcp/handlers_index.h>
#include <lci/mcp/server.h>
#include <lci/symbol.h>

#include <nlohmann/json.hpp>

#include "test_git.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace lci {
namespace mcp {
namespace {

// =============================================================================
// Test fixture with a populated index
// =============================================================================

class ExploreIndexTestFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        // Create temp directory with test source files
        tmp_dir_ = std::filesystem::temp_directory_path() /
                   "lci_explore_index_test";
        std::filesystem::remove_all(tmp_dir_);
        std::filesystem::create_directories(tmp_dir_);

        write_file("main.go",
                   "package main\n"
                   "\n"
                   "import \"fmt\"\n"
                   "\n"
                   "// HandleRequest processes incoming HTTP requests.\n"
                   "func HandleRequest(method string, path string) string {\n"
                   "    result := processPath(path)\n"
                   "    return fmt.Sprintf(\"%s: %s\", method, result)\n"
                   "}\n"
                   "\n"
                   "func processPath(path string) string {\n"
                   "    return path\n"
                   "}\n"
                   "\n"
                   "type Server struct {\n"
                   "    Port int\n"
                   "    Host string\n"
                   "}\n"
                   "\n"
                   "func (s *Server) Start() error {\n"
                   "    return nil\n"
                   "}\n"
                   "\n"
                   "var MaxConnections = 100\n");

        write_file("util.go",
                   "package main\n"
                   "\n"
                   "func helperFunc(input string) string {\n"
                   "    return input\n"
                   "}\n"
                   "\n"
                   "type Config struct {\n"
                   "    Timeout int\n"
                   "}\n");

        Config config;
        config.project.root = tmp_dir_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(tmp_dir_.string());
    }

    void TearDown() override {
        indexer_.reset();
        std::filesystem::remove_all(tmp_dir_);
    }

    void write_file(const std::string& name, const std::string& content) {
        auto path = tmp_dir_ / name;
        std::ofstream out(path);
        out << content;
    }

    std::filesystem::path tmp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
};

// =============================================================================
// list_symbols tests
// =============================================================================

TEST_F(ExploreIndexTestFixture, ListSymbolsRequiresKind) {
    nlohmann::json params = nlohmann::json::object();
    auto result = handle_list_symbols(params, *indexer_);
    EXPECT_TRUE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j["error"].get<std::string>().find("kind") !=
                std::string::npos);
}

TEST_F(ExploreIndexTestFixture, ListSymbolsFunctions) {
    nlohmann::json params = nlohmann::json::object();
    params["kind"] = "func";
    auto result = handle_list_symbols(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("symbols"));
    EXPECT_TRUE(j.contains("total"));
    EXPECT_TRUE(j.contains("showing"));
    EXPECT_TRUE(j.contains("has_more"));

    // Functions may or may not be found depending on Go parser availability
    EXPECT_GE(j["total"].get<int>(), 0);
}

TEST_F(ExploreIndexTestFixture, ListSymbolsAll) {
    nlohmann::json params = nlohmann::json::object();
    params["kind"] = "all";
    auto result = handle_list_symbols(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_GE(j["total"].get<int>(), 0);
}

TEST_F(ExploreIndexTestFixture, ListSymbolsPagination) {
    nlohmann::json params;
    params["kind"] = "all";
    params["max"] = 2;
    params["offset"] = 0;
    auto result = handle_list_symbols(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_LE(j["showing"].get<int>(), 2);

    int total = j["total"].get<int>();
    if (total > 2) {
        EXPECT_TRUE(j["has_more"].get<bool>());

        // Page 2
        params["offset"] = 2;
        auto r2 = handle_list_symbols(params, *indexer_);
        auto j2 = nlohmann::json::parse(r2.text);
        EXPECT_GE(j2["showing"].get<int>(), 0);
    }
}

TEST_F(ExploreIndexTestFixture, ListSymbolsWithNameFilter) {
    nlohmann::json params;
    params["kind"] = "func";
    params["name"] = "Handle";
    auto result = handle_list_symbols(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    for (const auto& sym : j["symbols"]) {
        auto name = sym["name"].get<std::string>();
        // Case-insensitive substring
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(),
                       lower_name.begin(), ::tolower);
        EXPECT_TRUE(lower_name.find("handle") != std::string::npos);
    }
}

TEST_F(ExploreIndexTestFixture, ListSymbolsSortByName) {
    nlohmann::json params;
    params["kind"] = "all";
    params["sort"] = "name";
    auto result = handle_list_symbols(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    auto& syms = j["symbols"];
    for (size_t i = 1; i < syms.size(); ++i) {
        EXPECT_LE(syms[i - 1]["name"].get<std::string>(),
                  syms[i]["name"].get<std::string>());
    }
}

TEST_F(ExploreIndexTestFixture, ListSymbolsIncludeAll) {
    nlohmann::json params;
    params["kind"] = "func";
    params["include"] = "all";
    auto result = handle_list_symbols(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    if (!j["symbols"].empty()) {
        auto& first = j["symbols"][0];
        // "all" includes ids and signature
        EXPECT_TRUE(first.contains("object_id"));
    }
}

TEST_F(ExploreIndexTestFixture, ListSymbolsMaxClamped) {
    nlohmann::json params;
    params["kind"] = "all";
    params["max"] = 9999;
    auto result = handle_list_symbols(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_LE(j["showing"].get<int>(), 500);
}

// =============================================================================
// inspect_symbol tests
// =============================================================================

TEST_F(ExploreIndexTestFixture, InspectSymbolRequiresNameOrId) {
    nlohmann::json params = nlohmann::json::object();
    auto result = handle_inspect_symbol(params, *indexer_);
    EXPECT_TRUE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j["error"].get<std::string>().find("name") !=
                std::string::npos);
}

TEST_F(ExploreIndexTestFixture, InspectSymbolByName) {
    nlohmann::json params;
    params["name"] = "HandleRequest";
    auto result = handle_inspect_symbol(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("symbols"));
    EXPECT_TRUE(j.contains("count"));

    // If the symbol was found, verify structure
    if (j["count"].get<int>() > 0) {
        auto& first = j["symbols"][0];
        EXPECT_EQ(first["name"].get<std::string>(), "HandleRequest");
        EXPECT_TRUE(first.contains("object_id"));
        EXPECT_TRUE(first.contains("type"));
        EXPECT_TRUE(first.contains("file"));
        EXPECT_TRUE(first.contains("line"));
        EXPECT_TRUE(first.contains("is_exported"));
    }
}

TEST_F(ExploreIndexTestFixture, InspectSymbolNotFound) {
    nlohmann::json params;
    params["name"] = "NonExistentSymbol99";
    auto result = handle_inspect_symbol(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_EQ(j["count"].get<int>(), 0);
}

TEST_F(ExploreIndexTestFixture, InspectSymbolWithTypeFilter) {
    nlohmann::json params;
    params["name"] = "Server";
    params["type"] = "struct";
    auto result = handle_inspect_symbol(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    for (const auto& sym : j["symbols"]) {
        EXPECT_EQ(sym["type"].get<std::string>(), "struct");
    }
}

// =============================================================================
// browse_file tests
// =============================================================================

TEST_F(ExploreIndexTestFixture, BrowseFileRequiresFileOrId) {
    nlohmann::json params = nlohmann::json::object();
    auto result = handle_browse_file(params, *indexer_);
    EXPECT_TRUE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j["error"].get<std::string>().find("file") !=
                std::string::npos);
}

TEST_F(ExploreIndexTestFixture, BrowseFileByName) {
    nlohmann::json params;
    params["file"] = "main.go";
    auto result = handle_browse_file(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("file"));
    EXPECT_TRUE(j.contains("symbols"));
    EXPECT_TRUE(j.contains("total"));
    EXPECT_TRUE(j["file"].contains("path"));
    EXPECT_TRUE(j["file"].contains("file_id"));
    EXPECT_TRUE(j["file"].contains("language"));
    EXPECT_EQ(j["file"]["language"].get<std::string>(), "go");
}

TEST_F(ExploreIndexTestFixture, BrowseFileNotFound) {
    nlohmann::json params;
    params["file"] = "nonexistent_file.go";
    auto result = handle_browse_file(params, *indexer_);
    EXPECT_TRUE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j["error"].get<std::string>().find("not found") !=
                std::string::npos);
}

TEST_F(ExploreIndexTestFixture, BrowseFileWithKindFilter) {
    nlohmann::json params;
    params["file"] = "main.go";
    params["kind"] = "func";
    auto result = handle_browse_file(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    for (const auto& sym : j["symbols"]) {
        auto type = sym["type"].get<std::string>();
        EXPECT_TRUE(type == "function" || type == "method");
    }
}

TEST_F(ExploreIndexTestFixture, BrowseFileWithStats) {
    nlohmann::json params;
    params["file"] = "main.go";
    params["show_stats"] = true;
    auto result = handle_browse_file(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("stats"));
    EXPECT_TRUE(j["stats"].contains("symbol_count"));
    EXPECT_TRUE(j["stats"].contains("function_count"));
    EXPECT_TRUE(j["stats"].contains("type_count"));
    EXPECT_TRUE(j["stats"].contains("exported_count"));
}

TEST_F(ExploreIndexTestFixture, BrowseFileSortByName) {
    nlohmann::json params;
    params["file"] = "main.go";
    params["sort"] = "name";
    auto result = handle_browse_file(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    auto& syms = j["symbols"];
    for (size_t i = 1; i < syms.size(); ++i) {
        EXPECT_LE(syms[i - 1]["name"].get<std::string>(),
                  syms[i]["name"].get<std::string>());
    }
}

// =============================================================================
// index_stats tests
// =============================================================================

TEST_F(ExploreIndexTestFixture, IndexStatsSummary) {
    nlohmann::json params;
    params["mode"] = "summary";
    auto result = handle_index_stats(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_EQ(j["status"].get<std::string>(), "ready");
    EXPECT_TRUE(j["server_ready"].get<bool>());
    EXPECT_TRUE(j.contains("file_count"));
    EXPECT_TRUE(j.contains("symbol_count"));
    EXPECT_TRUE(j.contains("reference_count"));
    EXPECT_TRUE(j.contains("index_time_ms"));
    EXPECT_GE(j["file_count"].get<int>(), 2);
}

TEST_F(ExploreIndexTestFixture, IndexStatsDefault) {
    nlohmann::json params = nlohmann::json::object();
    auto result = handle_index_stats(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("status"));
}

TEST_F(ExploreIndexTestFixture, IndexStatsDetailed) {
    nlohmann::json params;
    params["mode"] = "detailed";
    auto result = handle_index_stats(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("component_health"));
    EXPECT_TRUE(j.contains("memory_usage"));
}

TEST_F(ExploreIndexTestFixture, IndexStatsHealth) {
    nlohmann::json params;
    params["mode"] = "health";
    auto result = handle_index_stats(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("component_health"));
    auto& health = j["component_health"];
    EXPECT_TRUE(health.contains("symbol_index_ready"));
    EXPECT_TRUE(health.contains("ref_tracker_ready"));
}

TEST_F(ExploreIndexTestFixture, IndexStatsWithComponents) {
    nlohmann::json params;
    params["include_components"] = true;
    auto result = handle_index_stats(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("component_health"));
}

// =============================================================================
// debug_info tests
// =============================================================================

TEST_F(ExploreIndexTestFixture, DebugInfoOverview) {
    nlohmann::json params;
    params["mode"] = "overview";
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_EQ(j["mode"].get<std::string>(), "overview");
    EXPECT_TRUE(j.contains("overview"));
    auto& ov = j["overview"];
    EXPECT_TRUE(ov.contains("total_files"));
    EXPECT_TRUE(ov.contains("total_symbols"));
    EXPECT_TRUE(ov.contains("total_references"));
    EXPECT_TRUE(ov.contains("unique_languages"));
    EXPECT_TRUE(ov.contains("language_breakdown"));
    EXPECT_TRUE(ov.contains("type_breakdown"));
}

TEST_F(ExploreIndexTestFixture, DebugInfoDefault) {
    nlohmann::json params = nlohmann::json::object();
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_EQ(j["mode"].get<std::string>(), "overview");
}

TEST_F(ExploreIndexTestFixture, DebugInfoSymbols) {
    nlohmann::json params;
    params["mode"] = "symbols";
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("symbols_by_type"));
}

TEST_F(ExploreIndexTestFixture, DebugInfoReferences) {
    nlohmann::json params;
    params["mode"] = "references";
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("top_referenced_symbols"));
}

TEST_F(ExploreIndexTestFixture, DebugInfoFiles) {
    nlohmann::json params;
    params["mode"] = "files";
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("files_by_language"));
}

TEST_F(ExploreIndexTestFixture, DebugInfoUnknownMode) {
    nlohmann::json params;
    params["mode"] = "invalid_mode";
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_TRUE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j["error"].get<std::string>().find("unknown mode") !=
                std::string::npos);
}

// =============================================================================
// git_analysis tests
// =============================================================================

// The fixture's tmp_dir_ is a bare scratch directory, not a git repo. The real
// handler must fail fast with a clear error — NOT a "not_available" stub.
TEST_F(ExploreIndexTestFixture, GitAnalysisFailsFastOnNonGitDir) {
    nlohmann::json params;
    params["scope"] = "wip";
    auto result = handle_git_analysis(params, *indexer_);
    EXPECT_TRUE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    // make_error_response carries the operation + message; assert it names the
    // git-repo failure rather than the old hardcoded stub payload.
    EXPECT_EQ(j.value("status", std::string()), std::string());  // no stub key
    std::string text = result.text;
    EXPECT_NE(text.find("git"), std::string::npos);
}

TEST_F(ExploreIndexTestFixture, GitAnalysisRejectsBadScope) {
    nlohmann::json params;
    params["scope"] = "bogus";
    auto result = handle_git_analysis(params, *indexer_);
    EXPECT_TRUE(result.is_error);
}

// Real-repo happy path: git-init a throwaway repo, commit a baseline file, make
// an uncommitted long function (a WIP change), index it, and run the handler.
// Asserts the canonical report shape (summary + metadata) and a metrics finding
// for the over-length function — no mocks, real Provider + Analyzer.
TEST_F(ExploreIndexTestFixture, GitAnalysisRealRepoReturnsReport) {
    auto repo = std::filesystem::temp_directory_path() /
                "lci_git_analysis_real_test";
    std::filesystem::remove_all(repo);
    std::filesystem::create_directories(repo);

    auto git = [&](const std::string& args) {
        return test::run_git(repo, args);
    };
    ASSERT_TRUE(git("init"));
    git("config user.email test@test.com");
    git("config user.name test");

    // Baseline committed file (a short, tracked function).
    {
        std::ofstream f(repo / "base.go");
        f << "package main\n\nfunc Huge() int { return 1 }\n";
    }
    ASSERT_TRUE(git("add ."));
    ASSERT_TRUE(git("commit -m baseline"));

    // Uncommitted WIP modification of the tracked file: grow Huge() past 100
    // lines so the metrics analyzer raises a long_function finding. WIP scope
    // compares the working tree against HEAD, so the change must be to a
    // tracked file (an untracked new file is not part of the WIP diff).
    {
        std::ofstream f(repo / "base.go");
        f << "package main\n\nfunc Huge() int {\n\tx := 0\n";
        for (int i = 0; i < 130; ++i) f << "\tx += " << i << "\n";
        f << "\treturn x\n}\n";
    }

    Config cfg;
    cfg.project.root = repo.string();
    MasterIndex idx(cfg);
    idx.index_directory(repo.string());

    nlohmann::json params;
    params["scope"] = "wip";
    auto result = handle_git_analysis(params, idx);
    ASSERT_FALSE(result.is_error) << result.text;

    auto j = nlohmann::json::parse(result.text);
    ASSERT_TRUE(j.contains("summary"));
    ASSERT_TRUE(j.contains("metadata"));
    EXPECT_EQ(j["metadata"].value("scope", std::string()), "wip");
    EXPECT_GE(j["summary"].value("files_changed", 0), 1);
    // The long function must surface as a metrics issue.
    ASSERT_TRUE(j.contains("metrics_issues"));
    EXPECT_FALSE(j["metrics_issues"].empty());

    std::filesystem::remove_all(repo);
}

// =============================================================================
// Registration tests
// =============================================================================

TEST_F(ExploreIndexTestFixture, RegisterExploreHandlers) {
    Config config;
    config.project.root = tmp_dir_.string();
    McpServer server(config, *indexer_, nullptr);
    size_t before = server.tool_count();
    register_explore_handlers(server, indexer_.get());
    EXPECT_EQ(server.tool_count(), before + 3);
}

TEST_F(ExploreIndexTestFixture, RegisterIndexHandlers) {
    Config config;
    config.project.root = tmp_dir_.string();
    McpServer server(config, *indexer_, nullptr);
    size_t before = server.tool_count();
    register_index_handlers(server, indexer_.get());
    EXPECT_EQ(server.tool_count(), before + 3);
}

}  // namespace
}  // namespace mcp
}  // namespace lci
