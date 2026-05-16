#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/mcp/server.h>

#include <sstream>
#include <string>

namespace lci {
namespace mcp {
namespace {

// -- Helpers ------------------------------------------------------------------

/// Builds a JSON-RPC request string with Content-Length framing.
std::string frame_message(const nlohmann::json& msg) {
    auto body = msg.dump();
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
           body;
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

/// Parses a framed response from a string stream, returning the JSON body.
nlohmann::json parse_response(std::istream& stream) {
    std::string line;
    int content_length = -1;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) break;
        if (line.rfind("Content-Length:", 0) == 0) {
            content_length = std::stoi(line.substr(15));
        }
    }

    if (content_length <= 0) return nullptr;

    std::string body(static_cast<size_t>(content_length), '\0');
    stream.read(body.data(), content_length);
    return nlohmann::json::parse(body);
}

// -- Test fixture -------------------------------------------------------------

class McpServerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config config;
        config.project.root = "/tmp/lci-mcp-test";
        server_ = std::make_unique<McpServer>(config);
        server_->register_tools();
    }

    std::unique_ptr<McpServer> server_;
};

// -- Tool registration tests --------------------------------------------------

TEST_F(McpServerTest, RegistersAll14Tools) {
    EXPECT_EQ(server_->tool_count(), 14u);
    EXPECT_EQ(server_->tool_at(0).name, "info");
    EXPECT_EQ(server_->tool_at(1).name, "search");
    EXPECT_EQ(server_->tool_at(13).name, "browse_file");
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
        server_->register_tools();
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

    // Tools emit alphabetically-by-name to match Go jsonschema-go ordering
    // (FIX-D.2 Option B, MODULE_MAP.md "Decision: tools/list emit-order
    // parity"). First tool alphabetically across the 14 registered names is
    // "browse_file".
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

TEST_F(McpStdioTest, ToolCallStubResponse) {
    auto responses = exchange({
        make_request("initialize", 1),
        make_request(
            "tools/call", 2,
            {{"name", "search"},
             {"arguments", {{"pattern", "test"}}}}),
    });

    ASSERT_EQ(responses.size(), 2u);
    auto& call_resp = responses[1];
    EXPECT_EQ(call_resp["id"], 2);
    EXPECT_TRUE(call_resp.contains("result"));

    auto& content = call_resp["result"]["content"];
    ASSERT_EQ(content.size(), 1u);
    EXPECT_EQ(content[0]["type"], "text");

    auto text = nlohmann::json::parse(content[0]["text"].get<std::string>());
    EXPECT_EQ(text["status"], "not_implemented");
    EXPECT_EQ(text["tool"], "search");
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

    // Alphabetical-by-name (FIX-D.2 Option B emit order; MODULE_MAP.md
    // "Decision: tools/list emit-order parity"). The 14 tool names sorted
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
