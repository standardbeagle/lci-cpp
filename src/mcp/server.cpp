#include <lci/mcp/server.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

#include <lci/mcp/handlers_core.h>

namespace lci {
namespace mcp {

// -- McpServer construction ---------------------------------------------------

McpServer::McpServer(const Config& config)
    : config_(config),
      project_root_(detect_project_root(config)) {}

McpServer::McpServer(const Config& config, MasterIndex& indexer,
                     SearchEngine* search_engine)
    : config_(config),
      project_root_(detect_project_root(config)),
      indexer_(&indexer),
      search_engine_(search_engine) {}

McpServer::~McpServer() = default;

// -- Tool registration --------------------------------------------------------

void McpServer::add_tool(ToolDefinition def, ToolHandler handler) {
    registered_tools_.push_back({std::move(def), std::move(handler)});
}

size_t McpServer::tool_count() const { return registered_tools_.size(); }

const ToolDefinition& McpServer::tool_at(size_t index) const {
    return registered_tools_.at(index).definition;
}

// -- Stub handler for unimplemented tools ------------------------------------

namespace {

ToolResult stub_handler(const std::string& tool_name,
                        const nlohmann::json& /*params*/) {
    nlohmann::json data;
    data["status"] = "not_implemented";
    data["tool"] = tool_name;
    data["message"] = "Tool handler will be implemented in a subsequent task";
    return make_json_response(data);
}

}  // namespace

void McpServer::register_tools() {
    // 1. info — real handler (no indexer required; pure metadata).
    add_tool(
        {"info",
         "Get detailed help and examples for any tool. Use 'info' for "
         "overview or 'info <tool>' for specifics.",
         {{"tool", "string",
           "Tool name to get information about (e.g., 'search', "
           "'get_context', 'version')",
           ""}},
         {}},
        [](const nlohmann::json& p) { return handle_info(p); });

    // 2. search
    add_tool(
        {"search",
         "Sub-millisecond in-memory semantic code search. Use instead of "
         "grep, rg, find.",
         {{"pattern", "string", "Search pattern", ""},
          {"max", "integer", "Maximum results", ""},
          {"output", "string", "Output format", ""},
          {"filter", "string", "File filter", ""},
          {"flags", "string", "Search flags", ""},
          {"include", "string", "Include options", ""},
          {"symbol_types", "string",
           "Symbol types to filter (comma-separated)", ""},
          {"patterns", "string", "Multiple patterns (OR logic)", ""},
          {"max_per_file", "integer", "Max results per file", ""},
          {"semantic", "boolean", "Enable semantic search", ""},
          {"languages", "array",
           "Filter by programming languages", "string"}},
         {"pattern"}},
        [](const nlohmann::json& p) { return stub_handler("search", p); });

    // 3. get_context
    add_tool(
        {"get_context",
         "Get detailed context for specific code objects. Use 'id' with "
         "object IDs from search results.",
         {{"id", "string",
           "Concise object ID(s) from search results", ""},
          {"name", "string", "Symbol name for direct lookup", ""},
          {"file_id", "integer", "File ID to narrow lookup scope", ""},
          {"line", "integer", "Line number", ""},
          {"column", "integer", "Column number", ""},
          {"mode", "string", "Lookup mode", ""},
          {"include_full_symbol", "boolean", "Include full symbol info", ""},
          {"include_call_hierarchy", "boolean",
           "Include call hierarchy", ""},
          {"include_all_references", "boolean",
           "Include references", ""},
          {"include_dependencies", "boolean",
           "Include dependencies", ""},
          {"include_file_context", "boolean",
           "Include file context", ""},
          {"include_quality_metrics", "boolean",
           "Include quality metrics", ""},
          {"max_depth", "integer", "Max depth", ""},
          {"include_ai_text", "boolean", "Include AI text", ""},
          {"confidence_threshold", "number",
           "Confidence threshold", ""},
          {"exclude_test_files", "boolean",
           "Exclude test files", ""},
          {"include_sections", "array", "Include sections", "string"},
          {"exclude_sections", "array", "Exclude sections", "string"}},
         {}},
        [](const nlohmann::json& p) {
            return stub_handler("get_context", p);
        });

    // 4. semantic_annotations
    add_tool(
        {"semantic_annotations",
         "Query symbols by semantic labels or categories. Supports both "
         "direct annotations and propagated labels.",
         {{"label", "string", "Semantic label to search for", ""},
          {"category", "string", "Semantic category", ""},
          {"min_strength", "number", "Minimum label strength", ""},
          {"include_direct", "boolean",
           "Include direct annotations", ""},
          {"include_propagated", "boolean",
           "Include propagated labels", ""},
          {"max_results", "integer", "Maximum results", ""}},
         {}},
        [](const nlohmann::json& p) {
            return stub_handler("semantic_annotations", p);
        });

    // 5. side_effects
    add_tool(
        {"side_effects",
         "Query function purity and side effects. Detects writes, I/O, "
         "and exceptions with transitive analysis.",
         {{"mode", "string",
           "Query mode: symbol, file, pure, impure, category, summary", ""},
          {"symbol_id", "string", "Symbol ID for symbol mode", ""},
          {"symbol_name", "string", "Symbol name for symbol mode", ""},
          {"file_path", "string", "File path for file mode", ""},
          {"file_id", "integer", "File ID for file mode", ""},
          {"category", "string", "Side effect category", ""},
          {"include_reasons", "boolean",
           "Include reasons for impurity", ""},
          {"include_transitive", "boolean",
           "Include transitive side effects", ""},
          {"include_confidence", "boolean",
           "Include confidence levels", ""},
          {"max_results", "integer", "Maximum results", ""}},
         {}},
        [](const nlohmann::json& p) {
            return stub_handler("side_effects", p);
        });

    // 6. code_insight
    add_tool(
        {"code_insight",
         "Comprehensive codebase intelligence. Provides overview, detailed "
         "analysis, statistics, and git analysis.",
         {{"mode", "string", "Analysis mode", ""},
          {"tier", "integer", "Analysis tier", ""},
          {"analysis", "string", "Type of analysis", ""},
          {"metrics", "array", "Metrics to include", "string"},
          {"target", "string", "Target to analyze", ""},
          {"focus", "string", "Analysis focus", ""},
          {"max_results", "integer", "Maximum results", ""},
          {"languages", "array",
           "Filter by programming languages", "string"}},
         {}},
        [](const nlohmann::json& p) {
            return stub_handler("code_insight", p);
        });

    // 7. find_files
    add_tool(
        {"find_files",
         "Like 'find' or 'fd' - searches file paths on an in-memory "
         "index. Supports fuzzy matching and glob patterns.",
         {{"pattern", "string", "File/path pattern to search for", ""},
          {"max", "integer", "Maximum results (default: 50)", ""},
          {"filter", "string", "Filter by file type or glob", ""},
          {"flags", "string", "Search flags: 'ci', 'exact'", ""},
          {"include_hidden", "boolean",
           "Include hidden files/directories", ""},
          {"directory", "string",
           "Directory to search within", ""}},
         {"pattern"}},
        [](const nlohmann::json& p) {
            return stub_handler("find_files", p);
        });

    // 8. context
    add_tool(
        {"context",
         "Capture and hydrate code context manifests for agent handoff. "
         "Save compact symbol references, load for instant full context.",
         {{"operation", "string",
           "Operation: 'save' or 'load'", ""},
          {"refs", "array",
           "Code references to save (for 'save' operation)", "object"},
          {"to_file", "string", "Write manifest to file path", ""},
          {"to_string", "boolean",
           "Return manifest as JSON string", ""},
          {"append", "boolean", "Append to existing manifest", ""},
          {"task", "string", "Task description/directive", ""},
          {"from_file", "string",
           "Load manifest from file path", ""},
          {"from_string", "string",
           "Load manifest from inline JSON string", ""},
          {"filter", "array", "Only include these roles", "string"},
          {"exclude", "array", "Exclude these roles", "string"},
          {"format", "string",
           "Output format: 'full', 'signatures', 'outline'", ""},
          {"max_tokens", "integer",
           "Approximate token limit for hydrated context", ""}},
         {"operation"}},
        [](const nlohmann::json& p) {
            return stub_handler("context", p);
        });

    // 9. index_stats
    add_tool(
        {"index_stats",
         "Comprehensive index status and health monitoring. Shows "
         "indexing progress, component health, and memory usage.",
         {{"mode", "string",
           "Query mode: 'summary', 'detailed', 'progress', 'health'", ""},
          {"include_memory", "boolean",
           "Include memory usage statistics", ""},
          {"include_watch_mode", "boolean",
           "Include file watcher status", ""},
          {"include_components", "boolean",
           "Include per-component health", ""}},
         {}},
        [](const nlohmann::json& p) {
            return stub_handler("index_stats", p);
        });

    // 10. debug_info
    add_tool(
        {"debug_info",
         "Deep debug information for troubleshooting index issues. Shows "
         "symbol distribution and reference statistics.",
         {{"mode", "string",
           "Debug mode: 'overview', 'symbols', 'references', 'types', "
           "'files'",
           ""},
          {"file_id", "integer", "File ID to debug", ""},
          {"file_path", "string", "File path to debug", ""},
          {"max_results", "integer", "Maximum results", ""},
          {"verbose", "boolean", "Include detailed symbol info", ""}},
         {}},
        [](const nlohmann::json& p) {
            return stub_handler("debug_info", p);
        });

    // 11. git_analysis
    add_tool(
        {"git_analysis",
         "Analyze git changes for code quality issues. Finds duplicates, "
         "naming inconsistencies, and complexity issues.",
         {{"scope", "string",
           "Analysis scope: 'staged', 'wip', 'commit', 'range'", ""},
          {"base_ref", "string", "Base git reference", ""},
          {"target_ref", "string", "Target git reference", ""},
          {"focus", "array",
           "Analysis areas: 'duplicates', 'naming', 'metrics'", "string"},
          {"similarity_threshold", "number",
           "Similarity threshold for duplicate detection", ""},
          {"max_findings", "integer",
           "Maximum findings per category", ""}},
         {}},
        [](const nlohmann::json& p) {
            return stub_handler("git_analysis", p);
        });

    // 12. list_symbols
    add_tool(
        {"list_symbols",
         "Enumerate and filter symbols in the index. Like 'ls' for code: "
         "list functions, types, methods with filtering.",
         {{"kind", "string",
           "Symbol kinds (comma-separated): func, type, struct, "
           "interface, method, class, enum, variable, constant, all",
           ""},
          {"file", "string", "Glob pattern for file path filter", ""},
          {"exported", "boolean", "Visibility filter", ""},
          {"name", "string", "Substring filter on symbol name", ""},
          {"receiver", "string",
           "Filter methods by receiver type", ""},
          {"min_complexity", "integer",
           "Minimum cyclomatic complexity", ""},
          {"max_complexity", "integer",
           "Maximum cyclomatic complexity", ""},
          {"min_params", "integer", "Minimum parameter count", ""},
          {"max_params", "integer", "Maximum parameter count", ""},
          {"flags", "string",
           "Comma-separated flags: async, variadic, generator, method", ""},
          {"sort", "string",
           "Sort by: name, complexity, refs, line, params", ""},
          {"max", "integer", "Max results (default: 50)", ""},
          {"offset", "integer", "Pagination offset", ""},
          {"include", "string",
           "Extras: signature, doc, refs, callers, callees, scope, ids, "
           "all",
           ""}},
         {"kind"}},
        [](const nlohmann::json& p) {
            return stub_handler("list_symbols", p);
        });

    // 13. inspect_symbol
    add_tool(
        {"inspect_symbol",
         "Deep inspect a single symbol. Returns all metadata: signature, "
         "doc, complexity, callers, callees, type hierarchy.",
         {{"name", "string", "Symbol name (exact match)", ""},
          {"id", "string", "Object ID from search/list results", ""},
          {"file", "string",
           "File path pattern to disambiguate", ""},
          {"type", "string",
           "Symbol type to disambiguate", ""},
          {"include", "string",
           "Sections: signature, doc, callers, callees, "
           "type_hierarchy, scope, refs, annotations, flags, all",
           ""},
          {"max_depth", "integer",
           "Max depth for hierarchy traversal", ""}},
         {}},
        [](const nlohmann::json& p) {
            return stub_handler("inspect_symbol", p);
        });

    // 14. browse_file
    add_tool(
        {"browse_file",
         "Browse all symbols in a file - the outline view. Shows the "
         "complete symbol table with filtering and sorting.",
         {{"file", "string", "File path (exact, suffix, or glob)", ""},
          {"file_id", "integer", "File ID (alternative to path)", ""},
          {"kind", "string", "Filter by symbol kinds", ""},
          {"exported", "boolean", "Visibility filter", ""},
          {"sort", "string",
           "Sort by: line, name, type, complexity, refs", ""},
          {"max", "integer", "Max symbols (default: 100)", ""},
          {"include", "string",
           "Same as list_symbols. Default: signature,ids", ""},
          {"show_imports", "boolean", "Include import list", ""},
          {"show_stats", "boolean",
           "Include file-level statistics", ""}},
         {}},
        [](const nlohmann::json& p) {
            return stub_handler("browse_file", p);
        });
}

// -- Project root detection ---------------------------------------------------

std::string McpServer::detect_project_root(const Config& config) {
    if (!config.project.root.empty()) {
        return config.project.root;
    }

    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (!ec && !cwd.empty()) {
        return cwd.string();
    }

    return ".";
}

std::string McpServer::project_root() const { return project_root_; }

// -- Stdio JSON-RPC transport -------------------------------------------------

std::optional<nlohmann::json> McpServer::read_message() {
    // MCP uses Content-Length header framing (like LSP).
    // Format: "Content-Length: <N>\r\n\r\n<JSON body of N bytes>"
    std::string line;
    int content_length = -1;

    while (std::getline(std::cin, line)) {
        // Strip trailing \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            // Empty line separates headers from body
            break;
        }

        // Parse Content-Length header
        if (line.rfind("Content-Length:", 0) == 0) {
            auto value = line.substr(15);
            // Trim leading whitespace
            auto pos = value.find_first_not_of(' ');
            if (pos != std::string::npos) {
                value = value.substr(pos);
            }
            try {
                content_length = std::stoi(value);
            } catch (...) {
                return std::nullopt;
            }
        }
    }

    if (std::cin.eof() || content_length < 0) {
        return std::nullopt;
    }

    // Read exactly content_length bytes
    std::string body(static_cast<size_t>(content_length), '\0');
    std::cin.read(body.data(), content_length);

    if (std::cin.gcount() != content_length) {
        return std::nullopt;
    }

    try {
        return nlohmann::json::parse(body);
    } catch (...) {
        return std::nullopt;
    }
}

void McpServer::write_message(const nlohmann::json& msg) {
    auto body = msg.dump();
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::cout.flush();
}

// -- Request handling ---------------------------------------------------------

nlohmann::json McpServer::handle_request(const nlohmann::json& request) {
    auto method = request.value("method", "");
    auto id = request.contains("id") ? request["id"] : nlohmann::json(nullptr);

    // Notifications have no id - no response needed
    if (id.is_null() && request.contains("method") &&
        !request.contains("id")) {
        handle_notification(request);
        return nullptr;
    }

    nlohmann::json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;

    if (method == "initialize") {
        response["result"] = handle_initialize(request);
    } else if (method == "tools/list") {
        response["result"] = handle_tools_list(request);
    } else if (method == "tools/call") {
        auto params = request.value("params", nlohmann::json::object());
        auto tool_name = params.value("name", "");

        auto it = std::find_if(
            registered_tools_.begin(), registered_tools_.end(),
            [&](const RegisteredTool& reg) {
                return reg.definition.name == tool_name;
            });
        if (it == registered_tools_.end()) {
            response["error"] = {{"code", -32602},
                                 {"message", "unknown tool \"" + tool_name +
                                                 "\""}};
        } else {
            response["result"] = handle_tools_call(request);
        }
    } else if (method == "ping") {
        response["result"] = nlohmann::json::object();
    } else {
        response["error"] = {{"code", -32601},
                             {"message", "Method not found: " + method}};
    }

    return response;
}

nlohmann::json McpServer::handle_initialize(const nlohmann::json& /*request*/) {
    initialized_ = true;

    nlohmann::json result;
    result["protocolVersion"] = "2024-11-05";
    result["capabilities"]["tools"]["listChanged"] = false;
    result["serverInfo"]["name"] = "lci";
    result["serverInfo"]["version"] = "0.1.0";
    return result;
}

nlohmann::json McpServer::handle_tools_list(const nlohmann::json& /*request*/) {
    nlohmann::json tools = nlohmann::json::array();

    for (const auto& reg : registered_tools_) {
        nlohmann::json tool;
        tool["name"] = reg.definition.name;
        tool["description"] = reg.definition.description;
        tool["inputSchema"] = build_input_schema(reg.definition);
        tools.push_back(std::move(tool));
    }

    nlohmann::json result;
    result["tools"] = std::move(tools);
    return result;
}

nlohmann::json McpServer::handle_tools_call(const nlohmann::json& request) {
    auto params = request.value("params", nlohmann::json::object());
    auto tool_name = params.value("name", "");
    auto arguments = params.value("arguments", nlohmann::json::object());

    // Find the registered tool
    for (auto it = registered_tools_.rbegin(); it != registered_tools_.rend();
         ++it) {
        const auto& reg = *it;
        if (reg.definition.name == tool_name) {
            // Exception recovery wrapping
            try {
                auto result = reg.handler(arguments);

                nlohmann::json content_item;
                content_item["type"] = "text";
                content_item["text"] = result.text;

                nlohmann::json response;
                response["content"] = nlohmann::json::array({content_item});
                if (result.is_error) {
                    response["isError"] = true;
                }
                return response;
            } catch (const std::exception& e) {
                auto err = make_error_response(
                    tool_name,
                    std::string("Internal error: ") + e.what());

                nlohmann::json content_item;
                content_item["type"] = "text";
                content_item["text"] = err.text;

                nlohmann::json response;
                response["content"] = nlohmann::json::array({content_item});
                response["isError"] = true;
                return response;
            } catch (...) {
                auto err = make_error_response(
                    tool_name, "Unknown internal error");

                nlohmann::json content_item;
                content_item["type"] = "text";
                content_item["text"] = err.text;

                nlohmann::json response;
                response["content"] = nlohmann::json::array({content_item});
                response["isError"] = true;
                return response;
            }
        }
    }

    // Tool not found
    nlohmann::json response;
    response["isError"] = true;
    nlohmann::json content_item;
    content_item["type"] = "text";
    content_item["text"] =
        R"({"success":false,"error":"Unknown tool: )" + tool_name +
        R"(","operation":"tools/call"})";
    response["content"] = nlohmann::json::array({content_item});
    return response;
}

void McpServer::handle_notification(const nlohmann::json& request) {
    auto method = request.value("method", "");

    if (method == "notifications/initialized") {
        // Client acknowledged initialization - nothing to do
        return;
    }

    if (method == "notifications/cancelled") {
        // Request cancellation - not yet supported
        return;
    }

    // Unknown notification - silently ignore per JSON-RPC spec
}

// -- Schema building ----------------------------------------------------------

nlohmann::json McpServer::build_input_schema(const ToolDefinition& def) {
    nlohmann::json schema;
    schema["type"] = "object";

    nlohmann::json properties;
    for (const auto& prop : def.properties) {
        nlohmann::json p;
        p["type"] = prop.type;
        p["description"] = prop.description;
        if (prop.type == "array" && !prop.items_type.empty()) {
            p["items"]["type"] = prop.items_type;
        }
        properties[prop.name] = std::move(p);
    }
    schema["properties"] = std::move(properties);

    if (!def.required.empty()) {
        schema["required"] = def.required;
    }

    return schema;
}

// -- Main run loop ------------------------------------------------------------

int McpServer::run() {
    running_.store(true);

    while (running_.load()) {
        auto msg = read_message();
        if (!msg) {
            break;  // EOF or parse error
        }

        auto response = handle_request(*msg);
        if (!response.is_null()) {
            write_message(response);
        }
    }

    running_.store(false);
    return 0;
}

void McpServer::stop() { running_.store(false); }

}  // namespace mcp
}  // namespace lci
