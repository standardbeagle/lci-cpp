#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lci {
namespace mcp {

/// The tools one MCP session may list and call: `lci mcp --tools a,b,c`.
///
/// Every tools/list a client reads carries every schema it is handed, on every
/// request. A caller that grants a turn three tools wants three schemas, and a
/// client-side deny list cannot express that (it must name every tool, and
/// misses each tool added later). So the server narrows itself.
///
/// The allowlist belongs to the SESSION, not the server: the in-process host
/// also answers POST /mcp for other `lci mcp` processes bridged to it, and
/// each of those applies its own allowlist on its side of the bridge.
class ToolAllowlist {
  public:
    /// Parses a comma-separated `--tools` value, trimming whitespace around
    /// each name. Refuses an empty value and an empty name (`a,,b`, `a,`):
    /// an empty allowlist would serve nothing, and "every tool" is spelled by
    /// leaving --tools out. On refusal returns nullopt and fills `error`.
    static std::optional<ToolAllowlist> parse(std::string_view spec,
                                              std::string& error);

    /// "" when every allowlisted name is in `available` (the tools the
    /// session will actually be served); otherwise an error naming the
    /// unknown names and every available one.
    std::string unknown_tools_error(
        const std::vector<std::string>& available) const;

    bool allows(std::string_view name) const;

    /// Sorted, de-duplicated names.
    const std::vector<std::string>& names() const { return names_; }

    /// The JSON-RPC error object a tools/call outside the allowlist answers
    /// with: -32602, the same code as an unknown tool, naming the tools this
    /// session does serve.
    nlohmann::json refused_call_error(const std::string& name) const;

    /// The error a tools/call with these `params` earns in this session, or
    /// null: a tool outside the allowlist, or `info` asked to describe one
    /// (help for a tool the session refuses would invite a refused call).
    nlohmann::json call_refusal(const nlohmann::json& params) const;

    /// Narrows a tools/call response to the session: the `info` overview
    /// lists only the tools served. Every other response is left as is.
    void narrow_call_response(const std::string& tool_name,
                              nlohmann::json& response) const;

    /// Bridge side, request: when `request` is a tools/call (with an id)
    /// naming a tool outside the allowlist, the response frame to answer it
    /// with locally; otherwise "" and the frame is forwarded.
    std::string refused_call_frame(const nlohmann::json& request) const;

    /// Bridge side, response: drops tools outside the allowlist from a
    /// tools/list response frame, keeping the server's field order. A frame
    /// that is not a tools/list result is returned unchanged.
    std::string filter_tools_list_frame(const std::string& response) const;

    /// Bridge side, response: narrow_call_response on a tools/call frame.
    std::string narrow_call_response_frame(const std::string& tool_name,
                                           const std::string& response) const;

  private:
    explicit ToolAllowlist(std::vector<std::string> names)
        : names_(std::move(names)) {}

    std::vector<std::string> names_;
};

}  // namespace mcp
}  // namespace lci
