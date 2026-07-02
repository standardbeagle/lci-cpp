#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <lci/config.h>

namespace lci {

class MasterIndex;
class SearchEngine;

namespace mcp {

// -- Protocol versions --------------------------------------------------------

/// MCP protocol revisions this server supports, oldest first. initialize
/// echoes the client's requested version when supported, else the newest.
inline constexpr const char* kSupportedProtocolVersions[] = {
    "2024-11-05", "2025-03-26", "2025-06-18"};
inline constexpr const char* kLatestProtocolVersion = "2025-06-18";

// -- JSON-RPC types -----------------------------------------------------------

/// A single tool parameter property in a JSON Schema.
struct ToolProperty {
    std::string name;
    std::string type;        // "string", "integer", "number", "boolean", "array"
    std::string description;
    std::string items_type;  // For array types: the element type
    // Optional override for array items schema. When set (non-null), replaces
    // the default `{"type": items_type}` emission. Used to express complex
    // nested object schemas (e.g., context.refs items with sub-properties)
    // for Go parity without adding a recursive ToolProperty graph.
    nlohmann::json items_schema_override{};
};

/// Describes an MCP tool with its parameter schema.
struct ToolDefinition {
    std::string name;
    std::string description;
    std::vector<ToolProperty> properties;
    std::vector<std::string> required;
    // Accepted-but-undocumented parameter keys (e.g. legacy aliases a handler
    // normalizes internally). Not emitted in the tools/list inputSchema, but
    // permitted by the dispatch-level unknown-parameter guard so aliased calls
    // don't fail-fast. Leave empty for tools with no aliases.
    std::vector<std::string> aliases;
};

/// Result returned from a tool invocation.
struct ToolResult {
    std::string text;
    bool is_error{false};
};

/// Signature for a tool handler function.
/// Receives the parsed params JSON and returns a ToolResult.
using ToolHandler = std::function<ToolResult(const nlohmann::json& params)>;

// -- Response helpers ---------------------------------------------------------

/// Creates a JSON text response from arbitrary data.
ToolResult make_json_response(const nlohmann::json& data);

/// Creates an error response with structured error info.
ToolResult make_error_response(const std::string& operation,
                               const std::string& message);

// -- McpServer ----------------------------------------------------------------

/// MCP server with stdio JSON-RPC transport.
///
/// Reads newline-delimited JSON-RPC messages from stdin (MCP stdio framing),
/// writes responses to stdout. Implements the MCP protocol for tool
/// registration and invocation.
///
/// Thread safety: The server runs on a single thread (the calling thread
/// of run()). Not thread-safe for concurrent callers.
class McpServer {
  public:
    /// Creates a server with the given config.
    /// If root is empty, auto-detects the project root.
    explicit McpServer(const Config& config);

    /// Creates a server with an externally-managed index.
    McpServer(const Config& config, MasterIndex& indexer,
              SearchEngine* search_engine);

    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    /// Registers a single tool.
    void add_tool(ToolDefinition def, ToolHandler handler);

    /// Returns the number of registered tools.
    size_t tool_count() const;

    /// Returns the tool definition at the given index.
    const ToolDefinition& tool_at(size_t index) const;

    /// Runs the stdio transport loop. Blocks until EOF or error.
    /// Returns 0 on clean shutdown, non-zero on error.
    int run();

    /// Signals the server to stop after the current message.
    void stop();

    /// Returns the auto-detected or configured project root.
    std::string project_root() const;

  private:
    struct RegisteredTool {
        ToolDefinition definition;
        ToolHandler handler;
    };

    /// Reads a single JSON-RPC message from stdin.
    /// Returns nullopt on EOF.
    std::optional<nlohmann::json> read_message();

    /// Writes a JSON-RPC response to stdout.
    void write_message(const nlohmann::json& msg);

    /// Dispatches a JSON-RPC request and returns the response.
    nlohmann::json handle_request(const nlohmann::json& request);

    /// Handles initialize request.
    nlohmann::json handle_initialize(const nlohmann::json& request);

    /// Handles tools/list request.
    nlohmann::json handle_tools_list(const nlohmann::json& request);

    /// Executes an already-resolved tool with exception recovery, wrapping the
    /// handler result in the tools/call content envelope. Tool resolution
    /// (and the -32602 unknown-tool error) happens once in handle_request.
    nlohmann::json handle_tools_call(const RegisteredTool& tool,
                                     const nlohmann::json& arguments);

    /// Handles notifications (no response needed).
    void handle_notification(const nlohmann::json& request);

    /// Determines the project root with fallback logic.
    static std::string detect_project_root(const Config& config);

    /// Builds the JSON Schema for a tool definition.
    static nlohmann::json build_input_schema(const ToolDefinition& def);

    Config config_;
    std::string project_root_;
    MasterIndex* indexer_{};
    SearchEngine* search_engine_{};

    std::vector<RegisteredTool> registered_tools_;
    std::atomic<bool> running_{false};
    bool initialized_{false};
};

}  // namespace mcp
}  // namespace lci
