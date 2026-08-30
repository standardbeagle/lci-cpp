#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

/// Serializes JSON with U+FFFD replacement for invalid UTF-8 instead of the
/// nlohmann default, which throws type_error.316 on the first non-UTF-8 byte.
/// MCP responses reflect caller-supplied strings and source-derived text
/// (symbol names, code snippets, paths) that may contain non-UTF-8 bytes, so
/// every serialization that can see such content — tool payloads AND the wire
/// envelope written in the run loop — must use this to stay total. A strict
/// dump on the unwrapped wire path aborts the whole server.
std::string dump_json_lossy(const nlohmann::json& data);

/// Creates a JSON text response from arbitrary data.
ToolResult make_json_response(const nlohmann::json& data);

/// Creates an error response with structured error info. Reserve for caller
/// mistakes (bad params) and genuine internal failures — an isError result
/// reads as a code failure to agent callers.
ToolResult make_error_response(const std::string& operation,
                               const std::string& message);

/// Creates a successful not-applicable response for a well-formed request
/// whose environmental precondition is absent (non-git root, index still
/// warming, analyzer unpopulated, feature gated off): explicit
/// available=false + reason + hint, never isError and never fake empty data.
ToolResult make_unavailable_response(const std::string& operation,
                                     const std::string& reason,
                                     const std::string& hint);

// -- McpServer ----------------------------------------------------------------

/// MCP server with stdio JSON-RPC transport.
///
/// Reads newline-delimited JSON-RPC messages from stdin (MCP stdio framing),
/// writes responses to stdout. Implements the MCP protocol for tool
/// registration and invocation.
///
/// Thread safety: the transport loop runs on the calling thread of run();
/// tool calls execute serially on one internal worker thread so the
/// transport keeps answering initialize / tools/list / ping while a tool
/// call waits on index readiness. Handlers therefore never run
/// concurrently with each other. stdout writes are serialized internally.
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

    /// Installs a gate consulted before every tools/call. The gate runs on
    /// the internal tool worker thread, so it may block until the index is
    /// usable without stalling the transport: initialize / tools/list /
    /// ping keep answering while a tool call waits. (Before the worker
    /// existed the gate had to time out and error "still building", which
    /// made a cold one-shot client — pipe requests, close stdin — unable to
    /// EVER succeed on a large corpus: each retry was a new process and a
    /// full rebuild.)
    ///
    /// The gate blocks until the index is usable and returns true; on failure
    /// it fills `error` and returns false, and the call answers with that
    /// message instead of reaching a handler holding a half-built index.
    void set_readiness_gate(std::function<bool(std::string& error)> gate);

    /// Returns the number of registered tools.
    size_t tool_count() const;

    /// Returns the tool definition at the given index.
    const ToolDefinition& tool_at(size_t index) const;

    /// Dispatches one JSON-RPC message and returns the wire response line
    /// ("" for notifications / unparseable input). Unlike run(), this is
    /// synchronous and safe to call from any thread: tools/call waits the
    /// readiness gate and executes under the same serialization as the
    /// stdio worker. This is the entry point behind the index server's
    /// POST /mcp endpoint, which lets `lci mcp` bridge a stdio client to
    /// a shared, already-warmed server instead of re-indexing per process.
    std::string dispatch_wire(const std::string& line);

    /// Runs the stdio transport loop. Blocks until EOF or error; queued
    /// tool calls are drained (and their responses written) before it
    /// returns, so a one-shot client that closed stdin still gets every
    /// answer.
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

    /// One queued tools/call awaiting the worker thread.
    struct PendingCall {
        const RegisteredTool* tool;
        nlohmann::json arguments;
        nlohmann::json id;
    };

    /// Enqueues a tools/call for the worker thread (starts it on first use).
    void enqueue_call(PendingCall call);

    /// Worker loop: pops queued calls, waits the readiness gate, runs the
    /// handler, writes the response. Exits when told to finish and the
    /// queue is drained.
    void drain_calls();

    Config config_;
    std::string project_root_;
    MasterIndex* indexer_{};
    SearchEngine* search_engine_{};

    std::vector<RegisteredTool> registered_tools_;
    std::function<bool(std::string& error)> readiness_gate_;
    std::atomic<bool> running_{false};
    bool initialized_{false};

    /// Builds the ordered tools/list wire envelope (field order locked by
    /// mcp_server_test; shared by the stdio path and dispatch_wire).
    std::string tools_list_wire(const nlohmann::json& id) const;

    /// Runs the gate + handler for one resolved call and returns the full
    /// response envelope. Serialized by tool_mu_ across the stdio worker
    /// and dispatch_wire callers — handlers assume no concurrent peer.
    nlohmann::json execute_tool_call(const RegisteredTool& tool,
                                     const nlohmann::json& arguments,
                                     const nlohmann::json& id);

    std::mutex write_mu_;
    std::mutex tool_mu_;

    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::deque<PendingCall> queue_;
    bool finish_worker_{false};
    std::thread worker_;
};

}  // namespace mcp
}  // namespace lci
