#include <lci/mcp/server.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include <lci/mcp/handlers_core.h>
#include <lci/version.h>

namespace lci {
namespace mcp {

namespace {
// Forward declaration — defined below in file-local anonymous namespace.
// Builds inputSchema with Go jsonschema-go field order; consumed by
// handle_request's tools/list branch for wire output.
nlohmann::ordered_json build_input_schema_ordered(const ToolDefinition& def);
}  // namespace

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
    // MCP stdio transport: newline-delimited JSON-RPC, one message per line
    // (spec 2024-11-05 onward). Content-Length header framing is LSP, not
    // MCP — real clients (Claude Code, MCP Inspector) hang against it.
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        try {
            return nlohmann::json::parse(line);
        } catch (const nlohmann::json::parse_error& e) {
            // A malformed frame must not kill the server; JSON-RPC -32700
            // has no id to answer to, so surface on stderr and keep reading.
            std::cerr << "lci mcp: dropping unparseable input line: "
                      << e.what() << '\n';
        }
    }
    return std::nullopt;  // EOF
}

void McpServer::write_message(const nlohmann::json& msg) {
    // dump() escapes any embedded newlines, so the trailing '\n' is the
    // sole frame delimiter. Lossy UTF-8 so a response echoing non-UTF-8 bytes
    // (query text or source-derived content) can't abort the run loop on the
    // unwrapped wire write.
    std::cout << dump_json_lossy(msg) << '\n';
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
        // tools/list emits tools alphabetically by name with a fixed field
        // order (locked by mcp_server_test). Standard nlohmann::json
        // alphabetises on dump, so build the entire envelope
        // as ordered_json, serialise here, write directly, and return null to
        // signal the run loop to skip its default write_message path.
        nlohmann::ordered_json env;
        env["jsonrpc"] = "2.0";
        env["id"] = id;

        nlohmann::ordered_json result_obj;
        nlohmann::ordered_json tools_arr = nlohmann::ordered_json::array();

        // Snapshot pointers and sort alphabetically by tool name (Go emits
        // tools alphabetical-by-name; C++ registered_tools_ is in handler-file
        // insertion order). Sort the snapshot only — do NOT mutate
        // registered_tools_ (handle_tools_call iterates it directly; each tool
        // name is registered exactly once, so order is not load-bearing for
        // dispatch, but keep registered_tools_ stable regardless).
        std::vector<const RegisteredTool*> snapshot;
        snapshot.reserve(registered_tools_.size());
        for (const auto& reg : registered_tools_) {
            snapshot.push_back(&reg);
        }
        std::sort(snapshot.begin(), snapshot.end(),
                  [](const RegisteredTool* a, const RegisteredTool* b) {
                      return a->definition.name < b->definition.name;
                  });

        for (const auto* reg : snapshot) {
            nlohmann::ordered_json tool;
            // Tool object top-level keys: alphabetical [description,
            // inputSchema, name] (matches Go map iteration ordered by
            // jsonschema-go's sorted emit). Insert in sorted order.
            tool["description"] = reg->definition.description;
            tool["inputSchema"] = build_input_schema_ordered(reg->definition);
            tool["name"] = reg->definition.name;
            tools_arr.push_back(std::move(tool));
        }

        result_obj["tools"] = std::move(tools_arr);
        env["result"] = std::move(result_obj);

        std::cout << dump_json_lossy(env) << '\n';
        std::cout.flush();
        return nullptr;
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
            auto arguments =
                params.value("arguments", nlohmann::json::object());
            response["result"] = handle_tools_call(*it, arguments);
        }
    } else if (method == "ping") {
        response["result"] = nlohmann::json::object();
    } else {
        response["error"] = {{"code", -32601},
                             {"message", "Method not found: " + method}};
    }

    return response;
}

nlohmann::json McpServer::handle_initialize(const nlohmann::json& request) {
    initialized_ = true;

    // Version negotiation (MCP lifecycle): echo the client's requested
    // protocol version when we support it, else answer with our newest.
    auto params = request.value("params", nlohmann::json::object());
    auto requested = params.value("protocolVersion", "");
    const char* version = kLatestProtocolVersion;
    for (const char* v : kSupportedProtocolVersions) {
        if (requested == v) {
            version = v;
            break;
        }
    }

    nlohmann::json result;
    result["protocolVersion"] = version;
    result["capabilities"]["tools"]["listChanged"] = false;
    result["serverInfo"]["name"] = "lci";
    result["serverInfo"]["version"] = lci::kVersion;
    return result;
}

nlohmann::json McpServer::handle_tools_list(const nlohmann::json& /*request*/) {
    // This path is retained for header ABI compatibility and any direct
    // callers (none in production wire path). The wire response is emitted by
    // handle_request via build_tools_list_envelope_ordered() — that path
    // preserves Go's jsonschema-go field ordering on the dumped string.
    // Returning a regular nlohmann::json here will alphabetise inputSchema
    // keys on dump, which is fine for non-wire consumers.
    nlohmann::json tools = nlohmann::json::array();

    std::vector<const RegisteredTool*> snapshot;
    snapshot.reserve(registered_tools_.size());
    for (const auto& reg : registered_tools_) {
        snapshot.push_back(&reg);
    }
    std::sort(snapshot.begin(), snapshot.end(),
              [](const RegisteredTool* a, const RegisteredTool* b) {
                  return a->definition.name < b->definition.name;
              });

    for (const auto* reg : snapshot) {
        nlohmann::json tool;
        tool["name"] = reg->definition.name;
        tool["description"] = reg->definition.description;
        tool["inputSchema"] = build_input_schema(reg->definition);
        tools.push_back(std::move(tool));
    }

    nlohmann::json result;
    result["tools"] = std::move(tools);
    return result;
}

nlohmann::json McpServer::handle_tools_call(const RegisteredTool& tool,
                                            const nlohmann::json& arguments) {
    // Tool already resolved by handle_request (single lookup; unknown tools are
    // rejected there with JSON-RPC -32602). This only runs the handler and
    // wraps its result, catching handler exceptions so a single bad call never
    // takes down the stdio loop.

    // Unknown-parameter guard: reject typo'd / unsupported argument keys so a
    // mistyped param fails fast with a hint instead of being silently ignored
    // (karpathy #6). `search` owns richer JSON-Schema validation in its handler
    // (type/range/length checks + its own error shape), so it is exempt here.
    if (arguments.is_object() && tool.definition.name != "search") {
        std::string unknown;
        for (auto it = arguments.begin(); it != arguments.end(); ++it) {
            const auto& key = it.key();
            bool allowed = false;
            for (const auto& prop : tool.definition.properties) {
                if (prop.name == key) { allowed = true; break; }
            }
            if (!allowed) {
                for (const auto& a : tool.definition.aliases) {
                    if (a == key) { allowed = true; break; }
                }
            }
            if (!allowed) {
                if (!unknown.empty()) unknown += ", ";
                unknown += key;
            }
        }
        if (!unknown.empty()) {
            std::string allowed_list;
            for (const auto& prop : tool.definition.properties) {
                if (!allowed_list.empty()) allowed_list += ", ";
                allowed_list += prop.name;
            }
            auto err = make_error_response(
                tool.definition.name,
                "unknown parameter(s): " + unknown +
                    ". Allowed: " + allowed_list);
            nlohmann::json content_item;
            content_item["type"] = "text";
            content_item["text"] = err.text;
            nlohmann::json response;
            response["content"] = nlohmann::json::array({content_item});
            response["isError"] = true;
            return response;
        }
    }

    try {
        auto result = tool.handler(arguments);

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
            tool.definition.name, std::string("Internal error: ") + e.what());

        nlohmann::json content_item;
        content_item["type"] = "text";
        content_item["text"] = err.text;

        nlohmann::json response;
        response["content"] = nlohmann::json::array({content_item});
        response["isError"] = true;
        return response;
    } catch (...) {
        auto err = make_error_response(tool.definition.name,
                                       "Unknown internal error");

        nlohmann::json content_item;
        content_item["type"] = "text";
        content_item["text"] = err.text;

        nlohmann::json response;
        response["content"] = nlohmann::json::array({content_item});
        response["isError"] = true;
        return response;
    }
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

namespace {

// File-local: build inputSchema as ordered_json with Go-jsonschema-go field
// order. Returned ordered_json is consumed by the file-local
// build_tools_list_result_ordered() — never escapes the tools/list code path
// nor enters registered_tools_ storage (karpathy rule: keep ordered_json off
// the hot read path).
nlohmann::ordered_json build_input_schema_ordered(const ToolDefinition& def) {
    nlohmann::ordered_json schema;
    // Go jsonschema-go schema-root struct order: type → properties → required.
    schema["type"] = "object";

    // properties map keys: alphabetical (matches Go map iteration ordered by
    // jsonschema-go's sorted property emit). ordered_json preserves insertion
    // order, so insert in sorted order explicitly.
    std::vector<const ToolProperty*> sorted_props;
    sorted_props.reserve(def.properties.size());
    for (const auto& prop : def.properties) {
        sorted_props.push_back(&prop);
    }
    std::sort(sorted_props.begin(), sorted_props.end(),
              [](const ToolProperty* a, const ToolProperty* b) {
                  return a->name < b->name;
              });

    nlohmann::ordered_json properties = nlohmann::ordered_json::object();
    for (const auto* prop : sorted_props) {
        nlohmann::ordered_json p;
        // Go per-property struct order: type → description (+ items for arrays).
        p["type"] = prop->type;
        p["description"] = prop->description;
        if (prop->type == "array") {
            if (!prop->items_schema_override.is_null()) {
                // Caller-provided full items schema (used for complex
                // nested-object items, e.g. context.refs). Convert through
                // dump+parse so ordered_json preserves caller-specified key
                // order at every nesting level.
                p["items"] = nlohmann::ordered_json::parse(
                    prop->items_schema_override.dump());
            } else if (!prop->items_type.empty()) {
                nlohmann::ordered_json items;
                items["type"] = prop->items_type;
                p["items"] = std::move(items);
            }
        }
        properties[prop->name] = std::move(p);
    }
    schema["properties"] = std::move(properties);

    if (!def.required.empty()) {
        nlohmann::ordered_json req = nlohmann::ordered_json::array();
        for (const auto& r : def.required) {
            req.push_back(r);
        }
        schema["required"] = std::move(req);
    }

    return schema;
}

}  // namespace

// Retained for header ABI compatibility (declared in include/lci/mcp/server.h
// as `static nlohmann::json build_input_schema(...)`). The real ordered builder
// is the file-local build_input_schema_ordered() above; this wrapper exists so
// no callers/tests that bind to the public symbol break. Returns plain
// nlohmann::json (alphabetised on dump) — DO NOT use for tools/list wire
// output; handle_tools_list calls the ordered builder directly.
nlohmann::json McpServer::build_input_schema(const ToolDefinition& def) {
    return nlohmann::json::parse(build_input_schema_ordered(def).dump());
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
