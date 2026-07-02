#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/mcp/handlers_analysis.h>
#include <lci/mcp/handlers_context.h>
#include <lci/mcp/handlers_core.h>
#include <lci/mcp/handlers_explore.h>
#include <lci/mcp/handlers_index.h>
#include <lci/mcp/server.h>

#include <set>
#include <sstream>
#include <string>

namespace lci {
namespace mcp {
namespace {

// -- Helpers ------------------------------------------------------------------

/// Registers all 14 real tool definitions with null dependencies. The handler
/// lambdas capture the (null) deps and only fail at call time; registration
/// just installs the definitions — enough for server-mechanics / tools-list /
/// dispatch tests that don't drive a real index. Replaces the deleted stub
/// registrar (register_tools()/stub_handler).
void register_all_tools_nulldeps(McpServer& server) {
    register_core_handlers(server, nullptr, nullptr, nullptr);
    register_explore_handlers(server, nullptr);
    register_index_handlers(server, nullptr);
    register_analysis_handlers(server, nullptr, nullptr, nullptr, nullptr,
                               nullptr);
    register_context_handlers(server, nullptr);
}

/// Builds a JSON-RPC request string with newline-delimited framing (MCP stdio).
std::string frame_message(const nlohmann::json& msg) {
    return msg.dump() + "\n";
}

/// Builds a JSON-RPC request.
nlohmann::json make_request(const std::string& method, int id,
                            const nlohmann::json& params = nullptr) {
    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["id"] = id;
    req["method"] = method;
    if (!params.is_null()) {
        req["params"] = params;
    }
    return req;
}

/// Parses one newline-delimited response from a string stream.
nlohmann::json parse_response(std::istream& stream) {
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) continue;
        return nlohmann::json::parse(line);
    }
    return nullptr;
}

// -- Test fixture -------------------------------------------------------------

class McpServerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config config;
        config.project.root = "/tmp/lci-mcp-test";
        server_ = std::make_unique<McpServer>(config);
        register_all_tools_nulldeps(*server_);
    }

    std::unique_ptr<McpServer> server_;
};

// -- Tool registration tests --------------------------------------------------

TEST_F(McpServerTest, RegistersAll14Tools) {
    EXPECT_EQ(server_->tool_count(), 14u);
    // Registration order follows the register_*_handlers bundle order (not a
    // single registrar), so assert distinct names (no shadow double-registration
    // under forward-iter dispatch) + presence of the boundary tools, not order.
    std::set<std::string> names;
    for (size_t i = 0; i < server_->tool_count(); ++i) {
        names.insert(server_->tool_at(i).name);
    }
    EXPECT_EQ(names.size(), 14u) << "tool names must be unique (no shadows)";
    EXPECT_TRUE(names.count("info"));
    EXPECT_TRUE(names.count("search"));
    EXPECT_TRUE(names.count("browse_file"));
    EXPECT_TRUE(names.count("get_context"));
}

TEST_F(McpServerTest, ProjectRootFromConfig) {
    EXPECT_EQ(server_->project_root(), "/tmp/lci-mcp-test");
}

TEST_F(McpServerTest, ProjectRootAutoDetect) {
    Config config;
    // Empty root triggers auto-detection
    McpServer auto_server(config);
    EXPECT_FALSE(auto_server.project_root().empty());
    EXPECT_NE(auto_server.project_root(), ".");
}

// -- Stdio transport integration test -----------------------------------------
// These tests use stringstream redirection to simulate stdio.

class McpStdioTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config config;
        config.project.root = "/tmp/lci-mcp-test";
        server_ = std::make_unique<McpServer>(config);
        register_all_tools_nulldeps(*server_);
    }

    /// Sends requests via stdin simulation and captures stdout responses.
    /// Redirects cin/cout for the duration.
    std::vector<nlohmann::json> exchange(
        const std::vector<nlohmann::json>& requests) {
        // Build the input stream
        std::string input;
        for (const auto& req : requests) {
            input += frame_message(req);
        }

        // Save and redirect cin/cout
        auto* old_cin = std::cin.rdbuf();
        auto* old_cout = std::cout.rdbuf();

        std::istringstream in_stream(input);
        std::ostringstream out_stream;

        std::cin.rdbuf(in_stream.rdbuf());
        std::cout.rdbuf(out_stream.rdbuf());

        server_->run();

        // Restore
        std::cin.rdbuf(old_cin);
        std::cout.rdbuf(old_cout);

        // Parse responses
        std::vector<nlohmann::json> responses;
        std::istringstream response_stream(out_stream.str());
        while (response_stream.good() && response_stream.peek() != EOF) {
            auto resp = parse_response(response_stream);
            if (!resp.is_null()) {
                responses.push_back(std::move(resp));
            }
        }

        return responses;
    }

    std::unique_ptr<McpServer> server_;
};

TEST_F(McpStdioTest, InitializeHandshake) {
    auto responses = exchange({
        make_request("initialize", 1,
                     {{"protocolVersion", "2024-11-05"},
                      {"capabilities", nlohmann::json::object()},
                      {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}}),
    });

    ASSERT_EQ(responses.size(), 1u);
    auto& resp = responses[0];
    EXPECT_EQ(resp["id"], 1);
    EXPECT_TRUE(resp.contains("result"));
    EXPECT_EQ(resp["result"]["protocolVersion"], "2024-11-05");
    EXPECT_EQ(resp["result"]["serverInfo"]["name"], "lci");
}

TEST_F(McpStdioTest, InitializeEchoesSupportedProtocolVersion) {
    auto responses = exchange({
        make_request("initialize", 1,
                     {{"protocolVersion", "2025-06-18"},
                      {"capabilities", nlohmann::json::object()},
                      {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}}),
    });

    ASSERT_EQ(responses.size(), 1u);
    EXPECT_EQ(responses[0]["result"]["protocolVersion"], "2025-06-18");
}

TEST_F(McpStdioTest, InitializeFallsBackToLatestOnUnsupportedVersion) {
    auto responses = exchange({
        make_request("initialize", 1,
                     {{"protocolVersion", "1999-01-01"},
                      {"capabilities", nlohmann::json::object()},
                      {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}}),
    });

    ASSERT_EQ(responses.size(), 1u);
    EXPECT_EQ(responses[0]["result"]["protocolVersion"],
              kLatestProtocolVersion);
}

TEST_F(McpStdioTest, ToolsList) {
    auto responses = exchange({
        make_request("initialize", 1),
        make_request("tools/list", 2),
    });

    ASSERT_EQ(responses.size(), 2u);
    auto& tools_resp = responses[1];
    EXPECT_EQ(tools_resp["id"], 2);

    auto& tools = tools_resp["result"]["tools"];
    EXPECT_EQ(tools.size(), 14u);

    // Tools emit alphabetically by name. First tool alphabetically across the
    // 14 registered names is "browse_file".
    EXPECT_EQ(tools[0]["name"], "browse_file");
    EXPECT_TRUE(tools[0].contains("inputSchema"));

    // Verify alphabetical ordering across the whole array.
    for (size_t i = 1; i < tools.size(); ++i) {
        EXPECT_LE(tools[i - 1]["name"].get<std::string>(),
                  tools[i]["name"].get<std::string>())
            << "tools/list must be alphabetical-by-name at index " << i;
    }

    // Verify "search" tool exists and has required fields
    bool found_search = false;
    for (const auto& t : tools) {
        if (t["name"] == "search") {
            found_search = true;
            auto& schema = t["inputSchema"];
            EXPECT_TRUE(schema.contains("required"));
            auto& req = schema["required"];
            EXPECT_EQ(req.size(), 1u);
            EXPECT_EQ(req[0], "pattern");
            break;
        }
    }
    EXPECT_TRUE(found_search);
}

TEST_F(McpStdioTest, ToolCallDispatchesToHandler) {
    // Dispatch a real, dependency-free tool ("info") end-to-end through the
    // stdio transport. (The stub_handler that returned a fake "not_implemented"
    // payload is gone; indexer-backed tools would fault with the null-dep test
    // wiring, so exercise the metadata handler that needs no index.)
    auto responses = exchange({
        make_request("initialize", 1),
        make_request("tools/call", 2,
                     {{"name", "info"}, {"arguments", nlohmann::json::object()}}),
    });

    ASSERT_EQ(responses.size(), 2u);
    auto& call_resp = responses[1];
    EXPECT_EQ(call_resp["id"], 2);
    ASSERT_TRUE(call_resp.contains("result"));

    auto& content = call_resp["result"]["content"];
    ASSERT_EQ(content.size(), 1u);
    EXPECT_EQ(content[0]["type"], "text");
    EXPECT_FALSE(content[0]["text"].get<std::string>().empty());
    // Real handler -> not the old stub's not_implemented sentinel.
    EXPECT_EQ(call_resp["result"].value("isError", false), false);
}

TEST_F(McpStdioTest, ToolCallUnknownTool) {
    auto responses = exchange({
        make_request("initialize", 1),
        make_request(
            "tools/call", 2,
            {{"name", "nonexistent"},
             {"arguments", nlohmann::json::object()}}),
    });

    ASSERT_EQ(responses.size(), 2u);
    auto& call_resp = responses[1];
    EXPECT_TRUE(call_resp.contains("error"));
    EXPECT_EQ(call_resp["error"]["code"].get<int>(), -32602);
}

TEST_F(McpStdioTest, ExceptionRecovery) {
    // Register a tool that throws
    Config config;
    config.project.root = "/tmp/lci-mcp-test";
    auto throwing_server = std::make_unique<McpServer>(config);

    throwing_server->add_tool(
        {"throwing_tool", "A tool that throws", {}, {}},
        [](const nlohmann::json& /*params*/) -> ToolResult {
            throw std::runtime_error("deliberate test failure");
        });

    // Save/redirect cin/cout
    auto init_req = make_request("initialize", 1);
    auto call_req = make_request(
        "tools/call", 2,
        {{"name", "throwing_tool"},
         {"arguments", nlohmann::json::object()}});

    std::string input =
        frame_message(init_req) + frame_message(call_req);

    auto* old_cin = std::cin.rdbuf();
    auto* old_cout = std::cout.rdbuf();

    std::istringstream in_stream(input);
    std::ostringstream out_stream;

    std::cin.rdbuf(in_stream.rdbuf());
    std::cout.rdbuf(out_stream.rdbuf());

    int exit_code = throwing_server->run();

    std::cin.rdbuf(old_cin);
    std::cout.rdbuf(old_cout);

    EXPECT_EQ(exit_code, 0);

    // Parse responses and verify the error was caught
    std::istringstream response_stream(out_stream.str());
    std::vector<nlohmann::json> responses;
    while (response_stream.good() && response_stream.peek() != EOF) {
        auto resp = parse_response(response_stream);
        if (!resp.is_null()) responses.push_back(std::move(resp));
    }

    ASSERT_EQ(responses.size(), 2u);
    auto& err_resp = responses[1];
    EXPECT_TRUE(err_resp["result"]["isError"].get<bool>());

    auto text =
        nlohmann::json::parse(err_resp["result"]["content"][0]["text"]
                                  .get<std::string>());
    EXPECT_EQ(text["operation"], "throwing_tool");
    EXPECT_TRUE(text["error"].get<std::string>().find(
                    "deliberate test failure") != std::string::npos);
}

TEST_F(McpStdioTest, PingResponse) {
    auto responses = exchange({make_request("ping", 1)});

    ASSERT_EQ(responses.size(), 1u);
    EXPECT_EQ(responses[0]["id"], 1);
    EXPECT_TRUE(responses[0].contains("result"));
}

TEST_F(McpStdioTest, UnknownMethodReturnsError) {
    auto responses = exchange({make_request("nonexistent/method", 1)});

    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(responses[0].contains("error"));
    EXPECT_EQ(responses[0]["error"]["code"], -32601);
}

TEST_F(McpStdioTest, AllToolSchemasValid) {
    auto responses = exchange({
        make_request("initialize", 1),
        make_request("tools/list", 2),
    });

    ASSERT_EQ(responses.size(), 2u);
    auto& tools = responses[1]["result"]["tools"];

    for (const auto& tool : tools) {
        SCOPED_TRACE("Tool: " + tool["name"].get<std::string>());

        EXPECT_TRUE(tool.contains("name"));
        EXPECT_TRUE(tool.contains("description"));
        EXPECT_TRUE(tool.contains("inputSchema"));

        auto& schema = tool["inputSchema"];
        EXPECT_EQ(schema["type"], "object");
        EXPECT_TRUE(schema.contains("properties"));

        // Verify all properties have type and description
        for (auto& [key, val] : schema["properties"].items()) {
            SCOPED_TRACE("Property: " + key);
            EXPECT_TRUE(val.contains("type"));
            EXPECT_TRUE(val.contains("description"));
        }
    }
}

// -- Verify all 14 tool names --------------------------------------------------

TEST_F(McpStdioTest, All14ToolNamesPresent) {
    auto responses = exchange({
        make_request("initialize", 1),
        make_request("tools/list", 2),
    });

    ASSERT_EQ(responses.size(), 2u);
    auto& tools = responses[1]["result"]["tools"];

    // Alphabetical-by-name emit order. The 14 tool names sorted
    // lexicographically against the C++ registration set.
    std::vector<std::string> expected = {
        "browse_file",     "code_insight",   "context",
        "debug_info",      "find_files",     "get_context",
        "git_analysis",    "index_stats",    "info",
        "inspect_symbol",  "list_symbols",   "search",
        "semantic_annotations", "side_effects",
    };

    ASSERT_EQ(tools.size(), expected.size());

    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(tools[i]["name"], expected[i]);
    }
}

}  // namespace
}  // namespace mcp
}  // namespace lci
