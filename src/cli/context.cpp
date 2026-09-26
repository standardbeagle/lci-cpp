#include <lci/cli/commands.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lci {
namespace cli {

namespace {

// Splits one `--ref path:symbol:role` value into its three fields. There is
// no escaping language: a value is accepted only when it has exactly two
// colons and all three fields are non-empty. Anything else is ambiguous —
// colon-containing paths and `namespace::name` qualified symbols are the
// common cases — and is refused with guidance to the authoritative
// `--ref-json`, never guessed at.
bool split_ref_shorthand(const std::string& value, std::string& path,
                         std::string& symbol, std::string& role) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t colon = value.find(':', start);
        if (colon == std::string::npos) {
            fields.push_back(value.substr(start));
            break;
        }
        fields.push_back(value.substr(start, colon - start));
        start = colon + 1;
    }
    if (fields.size() != 3 || fields[0].empty() || fields[1].empty() ||
        fields[2].empty()) {
        return false;
    }
    path = fields[0];
    symbol = fields[1];
    role = fields[2];
    return true;
}

// Builds the `refs` argument array for the MCP context tool. `--ref-json`
// objects are authoritative and passed through verbatim; `--ref` shorthands
// are expanded only in their unambiguous three-field form. One malformed
// value fails the whole command rather than being dropped.
bool build_refs(const std::vector<std::string>& ref_json,
                const std::vector<std::string>& ref_shorthand,
                nlohmann::json& refs_out, std::string& error) {
    refs_out = nlohmann::json::array();
    for (const auto& raw : ref_json) {
        nlohmann::json ref;
        try {
            ref = nlohmann::json::parse(raw);
        } catch (const nlohmann::json::parse_error& e) {
            error = "invalid --ref-json value: " + std::string(e.what());
            return false;
        }
        if (!ref.is_object()) {
            error = "invalid --ref-json value: expected a reference object";
            return false;
        }
        refs_out.push_back(std::move(ref));
    }
    for (const auto& raw : ref_shorthand) {
        std::string path;
        std::string symbol;
        std::string role;
        if (!split_ref_shorthand(raw, path, symbol, role)) {
            error = "ambiguous --ref value '" + raw +
                    "': the shorthand is exactly 'path:symbol:role' with "
                    "three non-empty fields. Use --ref-json for qualified "
                    "symbols (namespace::name), paths containing ':', or any "
                    "other value.";
            return false;
        }
        refs_out.push_back(
            nlohmann::json{{"f", path}, {"s", symbol}, {"role", role}});
    }
    if (refs_out.empty()) {
        error = "at least one reference is required (use --ref-json or --ref)";
        return false;
    }
    return true;
}

// Resolves config and connects to (spawning if needed) the per-root index
// server. Returns 0 on success and prints the diagnostic to stderr on failure.
int connect_server(const GlobalFlags& flags, std::unique_ptr<Client>& out) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }
    std::string conn_err;
    out = ensure_server_running(cfg, flags, conn_err);
    if (!out) {
        std::cerr << "Error: " << conn_err << "\n";
        return 1;
    }
    return 0;
}

// Sends one `context` tools/call through the server's POST /mcp bridge and
// unwraps the inner JSON result. Returns 0 when a structured result was
// received (including one carrying `isError`, reported via `is_error`),
// nonzero on transport, framing, or JSON-RPC error. The CLI never parses,
// hydrates, persists, or budget-accounts a manifest itself.
int dispatch_context(Client& client, const nlohmann::json& arguments,
                     nlohmann::json& result_out, bool& is_error,
                     std::string& error) {
    const nlohmann::json request = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {{"name", "context"}, {"arguments", arguments}}}};

    std::string response;
    std::string transport_error;
    const int status =
        client.mcp_dispatch(request.dump(), response, transport_error);
    if (status < 0) {
        error = "context request failed: " + transport_error;
        return 1;
    }
    if (status != 200) {
        error = "context request failed: server returned HTTP " +
                std::to_string(status) +
                (response.empty() ? "" : ": " + response);
        return 1;
    }

    nlohmann::json envelope;
    try {
        envelope = nlohmann::json::parse(response);
    } catch (const nlohmann::json::parse_error& e) {
        error = "context request failed: invalid JSON-RPC response: " +
                std::string(e.what());
        return 1;
    }
    if (envelope.contains("error")) {
        error = "context request failed: " + envelope["error"].dump();
        return 1;
    }
    if (!envelope.contains("result") || !envelope["result"].is_object()) {
        error = "context request failed: response carried no result";
        return 1;
    }

    const auto& result = envelope["result"];
    is_error = result.value("isError", false);
    std::string text;
    if (result.contains("content") && result["content"].is_array()) {
        for (const auto& block : result["content"]) {
            if (block.value("type", "") == "text") {
                text += block.value("text", "");
            }
        }
    }
    if (text.empty()) {
        error = "context request failed: response carried no text content";
        return 1;
    }
    try {
        result_out = nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error& e) {
        error = "context request failed: tool result is not JSON: " +
                std::string(e.what());
        return 1;
    }
    return 0;
}

// Renders a structured tool error to stderr and returns the CLI exit code.
int report_tool_error(const nlohmann::json& result, const char* fallback) {
    std::cerr << "Error: " << result.value("error", std::string(fallback))
              << "\n";
    return 1;
}

}  // namespace

int run_context_save(const GlobalFlags& flags,
                     const ContextSaveOptions& options) {
    nlohmann::json refs;
    std::string ref_error;
    if (!build_refs(options.ref_json, options.ref_shorthand, refs, ref_error)) {
        std::cerr << "Error: " << ref_error << "\n";
        return 1;
    }

    std::unique_ptr<Client> client;
    if (const int rc = connect_server(flags, client); rc != 0) return rc;

    nlohmann::json arguments = {
        {"operation", "save"},
        {"refs", std::move(refs)},
        {"to_file", options.output},
    };
    if (!options.task.empty()) arguments["task"] = options.task;

    nlohmann::json result;
    bool is_error = false;
    std::string dispatch_error;
    if (dispatch_context(*client, arguments, result, is_error,
                         dispatch_error) != 0) {
        std::cerr << "Error: " << dispatch_error << "\n";
        return 1;
    }
    if (is_error) return report_tool_error(result, "context save failed");

    std::cout << result.dump(2) << "\n";
    return 0;
}

int run_context_load(const GlobalFlags& flags,
                     const ContextLoadOptions& options) {
    // The CLI11 check already rejects a non-positive budget; this is the same
    // contract at the call boundary, so an in-process caller cannot bypass it.
    if (options.max_tokens < 1) {
        std::cerr << "Error: --max-tokens must be a positive integer\n";
        return 1;
    }

    std::unique_ptr<Client> client;
    if (const int rc = connect_server(flags, client); rc != 0) return rc;

    const nlohmann::json arguments = {
        {"operation", "load"},
        {"from_file", options.path},
        {"max_tokens", options.max_tokens},
    };

    nlohmann::json result;
    bool is_error = false;
    std::string dispatch_error;
    if (dispatch_context(*client, arguments, result, is_error,
                         dispatch_error) != 0) {
        std::cerr << "Error: " << dispatch_error << "\n";
        return 1;
    }
    if (is_error) return report_tool_error(result, "context load failed");

    // The index was not ready when the request arrived: a diagnostic, not a
    // structured context result.
    if (result.value("available", true) == false) {
        std::cerr << "Error: " << result.value("reason", "context unavailable");
        if (result.contains("hint") && result["hint"].is_string()) {
            std::cerr << " (" << result["hint"].get<std::string>() << ")";
        }
        std::cerr << "\n";
        return 1;
    }

    // A load whose refs only partially resolve is still a successful
    // structured response: the `unresolved` array and stats carry the failure
    // to the caller, and the exit is zero.
    std::cout << result.dump(2) << "\n";
    return 0;
}

}  // namespace cli
}  // namespace lci
