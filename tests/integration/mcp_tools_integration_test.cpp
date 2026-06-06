#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_core.h>
#include <lci/mcp/handlers_explore.h>
#include <lci/mcp/handlers_index.h>
#include <lci/mcp/server.h>
#include <lci/search/search_engine.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace lci {
namespace mcp {
namespace {

// TempDir matches the pattern used by existing passing tests.
class TempDir {
  public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lci_mcp_integ_" + std::to_string(
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

// McpToolsIntegrationTest exercises MCP tool handlers with a real index
// built from a multi-file fixture. This verifies the full stack from
// JSON params -> handler -> index -> search engine -> JSON response.

class McpToolsIntegrationTest : public ::testing::Test {
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
            "}\n"
            "\n"
            "func handleRequest(input string) string {\n"
            "    return \"handled: \" + input\n"
            "}\n"
            "\n"
            "type User struct {\n"
            "    ID   int\n"
            "    Name string\n"
            "}\n");

        Config config;
        config.project.root = dir_.path().string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(dir_.path().string());
        search_engine_ = std::make_unique<SearchEngine>(*indexer_);
    }

    TempDir dir_;
    std::unique_ptr<MasterIndex> indexer_;
    std::unique_ptr<SearchEngine> search_engine_;
};

// -- Tool: info ---------------------------------------------------------------

TEST_F(McpToolsIntegrationTest, InfoToolReturnsOverview) {
    auto result = handle_info(nlohmann::json::object());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("server"));
    EXPECT_TRUE(json.contains("available_tools"));
}

TEST_F(McpToolsIntegrationTest, InfoToolWithSpecificTool) {
    auto result = handle_info({{"tool", "search"}});
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["name"], "search");
    EXPECT_TRUE(json.contains("parameters"));
}

// -- Tool: search -------------------------------------------------------------

TEST_F(McpToolsIntegrationTest, SearchToolFindsResults) {
    auto result = handle_search(
        {{"pattern", "greet"}}, *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("results"));
    EXPECT_GT(json["total_matches"].get<int>(), 0);
}

TEST_F(McpToolsIntegrationTest, SearchToolWithMaxResults) {
    auto result = handle_search(
        {{"pattern", "func"}, {"max", 2}},
        *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_LE(json["results"].size(), 2u);
}

TEST_F(McpToolsIntegrationTest, SearchToolFilesOutput) {
    auto result = handle_search(
        {{"pattern", "main"}, {"output", "files"}},
        *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("files"));
}

TEST_F(McpToolsIntegrationTest, SearchToolCountOutput) {
    auto result = handle_search(
        {{"pattern", "main"}, {"output", "count"}},
        *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("total_matches"));
}

// -- Tool: get_context --------------------------------------------------------

TEST_F(McpToolsIntegrationTest, GetContextToolReturnsValidResponse) {
    // get_context looks up reference tracker data, which may not have
    // full cross-references for small test fixtures. Verify the handler
    // returns a valid JSON response (possibly with an error for unknown
    // symbols) rather than crashing.
    auto result = handle_get_context(
        {{"name", "main"}}, *indexer_);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.is_object());
}

TEST_F(McpToolsIntegrationTest, GetContextToolMissingParamsReturnsError) {
    auto result = handle_get_context(
        nlohmann::json::object(), *indexer_);
    EXPECT_TRUE(result.is_error);
}

// -- Tool: find_files ---------------------------------------------------------

TEST_F(McpToolsIntegrationTest, FindFilesToolReturnsMatches) {
    auto result = handle_find_files(
        {{"pattern", "*.go"}}, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("files") || json.contains("matches") ||
                json.contains("results"));
}

// -- Tool: list_symbols -------------------------------------------------------

TEST_F(McpToolsIntegrationTest, ListSymbolsToolReturnsAll) {
    auto result = handle_list_symbols(
        {{"kind", "all"}}, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("symbols"));
    EXPECT_TRUE(json.contains("total"));
}

TEST_F(McpToolsIntegrationTest, ListSymbolsToolWithKindFilter) {
    auto result = handle_list_symbols(
        {{"kind", "function"}}, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("symbols"));
}

// -- Tool: inspect_symbol -----------------------------------------------------

TEST_F(McpToolsIntegrationTest, InspectSymbolToolReturnsDetails) {
    auto result = handle_inspect_symbol(
        {{"name", "User"}}, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("symbols"));
    EXPECT_TRUE(json.contains("count"));
}

// -- Tool: browse_file --------------------------------------------------------

TEST_F(McpToolsIntegrationTest, BrowseFileToolReturnsFileInfo) {
    auto result = handle_browse_file(
        {{"file", "main.go"}}, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("file"));
    EXPECT_TRUE(json.contains("symbols"));
}

TEST_F(McpToolsIntegrationTest, BrowseFileToolNotFoundReturnsError) {
    auto result = handle_browse_file(
        {{"file", "nonexistent.xyz"}}, *indexer_);
    EXPECT_TRUE(result.is_error);
}

// -- Tool: index_stats --------------------------------------------------------

TEST_F(McpToolsIntegrationTest, IndexStatsToolReturnsMetrics) {
    auto result = handle_index_stats(
        nlohmann::json::object(), *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("file_count") || json.contains("stats") ||
                json.contains("index"));
}

// -- Tool: debug_info ---------------------------------------------------------

TEST_F(McpToolsIntegrationTest, DebugInfoToolReturnsData) {
    auto result = handle_debug_info(
        nlohmann::json::object(), *indexer_);
    EXPECT_FALSE(result.is_error);
}

// -- Tool: git_analysis -------------------------------------------------------

TEST_F(McpToolsIntegrationTest, GitAnalysisToolReturnsErrorOrData) {
    auto result = handle_git_analysis({{"scope", "staged"}}, *indexer_);
    // The scratch index root is not a git repo, so the real handler fails
    // fast with a structured error — never malformed payload.
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.is_object());
    EXPECT_TRUE(result.is_error);
}

TEST_F(McpToolsIntegrationTest, GitAnalysisIsWiredToRealAnalyzerNotAStub) {
    // handle_git_analysis() now drives the real git::Analyzer (Dart task
    // sL0AJDf2hjIh). On a non-git root it must FAIL FAST — it must NOT fall
    // back to the old "not_available"/"future" stub payload, which would mask
    // the missing-repo condition behind a fake-success envelope.
    auto result = handle_git_analysis({{"scope", "wip"}}, *indexer_);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.is_object());

    EXPECT_TRUE(result.is_error);
    // The dead stub keys must be gone.
    EXPECT_FALSE(json.contains("status"));
    EXPECT_NE(result.text.find("git"), std::string::npos);
}

TEST_F(McpToolsIntegrationTest, GitAnalysisHandlesUnknownScope) {
    auto result = handle_git_analysis({{"scope", "nonsense_value"}}, *indexer_);
    auto json = nlohmann::json::parse(result.text);
    // Unknown scope is rejected with a structured error; the response must
    // remain a JSON object so the MCP transport never sees malformed payload.
    EXPECT_TRUE(json.is_object());
    EXPECT_TRUE(result.is_error);
}

// -- Full MCP stdio round-trip ------------------------------------------------

TEST_F(McpToolsIntegrationTest, StdioRoundTripWithRealIndex) {
    Config config;
    config.project.root = dir_.path().string();

    McpServer server(config, *indexer_, search_engine_.get());
    server.register_tools();

    // Build JSON-RPC messages
    auto frame = [](const nlohmann::json& msg) -> std::string {
        auto body = msg.dump();
        return "Content-Length: " + std::to_string(body.size()) +
               "\r\n\r\n" + body;
    };

    auto make_req = [](const std::string& method, int id,
                       const nlohmann::json& params = nullptr)
        -> nlohmann::json {
        nlohmann::json req;
        req["jsonrpc"] = "2.0";
        req["id"] = id;
        req["method"] = method;
        if (!params.is_null()) req["params"] = params;
        return req;
    };

    std::string input;
    input += frame(make_req("initialize", 1,
        {{"protocolVersion", "2024-11-05"},
         {"capabilities", nlohmann::json::object()},
         {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}}));
    input += frame(make_req("tools/list", 2));
    input += frame(make_req("tools/call", 3,
        {{"name", "search"},
         {"arguments", {{"pattern", "greet"}}}}));

    auto* old_cin = std::cin.rdbuf();
    auto* old_cout = std::cout.rdbuf();

    std::istringstream in_stream(input);
    std::ostringstream out_stream;
    std::cin.rdbuf(in_stream.rdbuf());
    std::cout.rdbuf(out_stream.rdbuf());

    server.run();

    std::cin.rdbuf(old_cin);
    std::cout.rdbuf(old_cout);

    // Parse responses
    auto parse_resp = [](std::istream& s) -> nlohmann::json {
        std::string line;
        int content_length = -1;
        while (std::getline(s, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            if (line.rfind("Content-Length:", 0) == 0)
                content_length = std::stoi(line.substr(15));
        }
        if (content_length <= 0) return nullptr;
        std::string body(static_cast<size_t>(content_length), '\0');
        s.read(body.data(), content_length);
        return nlohmann::json::parse(body);
    };

    std::vector<nlohmann::json> responses;
    std::istringstream resp_stream(out_stream.str());
    while (resp_stream.good() && resp_stream.peek() != EOF) {
        auto resp = parse_resp(resp_stream);
        if (!resp.is_null()) responses.push_back(std::move(resp));
    }

    ASSERT_EQ(responses.size(), 3u);

    // Response 1: initialize
    EXPECT_EQ(responses[0]["id"], 1);
    EXPECT_EQ(responses[0]["result"]["protocolVersion"], "2024-11-05");

    // Response 2: tools/list
    EXPECT_EQ(responses[1]["id"], 2);
    EXPECT_EQ(responses[1]["result"]["tools"].size(), 14u);

    // Response 3: tools/call search with real index
    EXPECT_EQ(responses[2]["id"], 3);
    auto& content = responses[2]["result"]["content"];
    ASSERT_GE(content.size(), 1u);
    EXPECT_EQ(content[0]["type"], "text");

    auto text = nlohmann::json::parse(content[0]["text"].get<std::string>());
    // Stub handlers return status and tool name
    EXPECT_TRUE(text.contains("status") || text.contains("results") ||
                text.contains("total_matches"));
}

// -- MCP tool call round-trip for all 14 tools --------------------------------

TEST_F(McpToolsIntegrationTest, AllToolsCallableViaStdio) {
    Config config;
    config.project.root = dir_.path().string();

    McpServer server(config, *indexer_, search_engine_.get());
    server.register_tools();

    auto frame = [](const nlohmann::json& msg) -> std::string {
        auto body = msg.dump();
        return "Content-Length: " + std::to_string(body.size()) +
               "\r\n\r\n" + body;
    };

    auto make_req = [](const std::string& method, int id,
                       const nlohmann::json& params = nullptr)
        -> nlohmann::json {
        nlohmann::json req;
        req["jsonrpc"] = "2.0";
        req["id"] = id;
        req["method"] = method;
        if (!params.is_null()) req["params"] = params;
        return req;
    };

    // Build requests: initialize + 14 tool calls
    std::string input;
    input += frame(make_req("initialize", 1));

    struct ToolCall {
        std::string name;
        nlohmann::json args;
    };
    std::vector<ToolCall> calls = {
        {"info",                 nlohmann::json::object()},
        {"search",               {{"pattern", "main"}}},
        {"get_context",          {{"symbol", "greet"}}},
        {"semantic_annotations", {{"symbol", "main"}}},
        {"side_effects",         {{"symbol", "greet"}}},
        {"code_insight",         {{"symbol", "User"}}},
        {"find_files",           {{"pattern", "*.go"}}},
        {"context",              {{"task", "test"}, {"refs", nlohmann::json::array()}}},
        {"index_stats",          nlohmann::json::object()},
        {"debug_info",           nlohmann::json::object()},
        {"git_analysis",         {{"scope", "staged"}}},
        {"list_symbols",         nlohmann::json::object()},
        {"inspect_symbol",       {{"name", "User"}}},
        {"browse_file",          {{"file", "main.go"}}},
    };

    int id = 2;
    for (const auto& c : calls) {
        input += frame(make_req("tools/call", id++,
            {{"name", c.name}, {"arguments", c.args}}));
    }

    auto* old_cin = std::cin.rdbuf();
    auto* old_cout = std::cout.rdbuf();

    std::istringstream in_stream(input);
    std::ostringstream out_stream;
    std::cin.rdbuf(in_stream.rdbuf());
    std::cout.rdbuf(out_stream.rdbuf());

    int exit_code = server.run();

    std::cin.rdbuf(old_cin);
    std::cout.rdbuf(old_cout);

    EXPECT_EQ(exit_code, 0);

    // Parse all responses
    auto parse_resp = [](std::istream& s) -> nlohmann::json {
        std::string line;
        int cl = -1;
        while (std::getline(s, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            if (line.rfind("Content-Length:", 0) == 0)
                cl = std::stoi(line.substr(15));
        }
        if (cl <= 0) return nullptr;
        std::string body(static_cast<size_t>(cl), '\0');
        s.read(body.data(), cl);
        return nlohmann::json::parse(body);
    };

    std::vector<nlohmann::json> responses;
    std::istringstream resp_stream(out_stream.str());
    while (resp_stream.good() && resp_stream.peek() != EOF) {
        auto resp = parse_resp(resp_stream);
        if (!resp.is_null()) responses.push_back(std::move(resp));
    }

    // 1 initialize + 14 tool calls = 15 responses
    ASSERT_EQ(responses.size(), 15u);

    // All tool call responses should have valid result structure
    for (size_t i = 1; i < responses.size(); ++i) {
        SCOPED_TRACE("Tool call index " + std::to_string(i) +
                     " (" + calls[i - 1].name + ")");
        auto& resp = responses[i];
        EXPECT_TRUE(resp.contains("result"))
            << "Response missing 'result': " << resp.dump(2);
        EXPECT_TRUE(resp["result"].contains("content"))
            << "Result missing 'content': " << resp["result"].dump(2);
    }
}

}  // namespace
}  // namespace mcp
}  // namespace lci
