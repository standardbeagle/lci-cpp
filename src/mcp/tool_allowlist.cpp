#include <lci/mcp/tool_allowlist.h>

#include <algorithm>

#include <lci/mcp/server.h>

namespace lci {
namespace mcp {

namespace {

std::string_view trim(std::string_view s) {
    constexpr std::string_view kSpace = " \t\r\n";
    const auto first = s.find_first_not_of(kSpace);
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(kSpace);
    return s.substr(first, last - first + 1);
}

std::string join(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) {
        if (!out.empty()) out += ", ";
        out += n;
    }
    return out;
}

}  // namespace

std::optional<ToolAllowlist> ToolAllowlist::parse(std::string_view spec,
                                                  std::string& error) {
    if (trim(spec).empty()) {
        error = "--tools needs at least one tool name; omit --tools to serve "
                "every tool";
        return std::nullopt;
    }
    std::vector<std::string> names;
    for (size_t start = 0;;) {
        const auto comma = spec.find(',', start);
        const auto name = trim(spec.substr(
            start, comma == std::string_view::npos ? std::string_view::npos
                                                   : comma - start));
        if (name.empty()) {
            error = "--tools has an empty tool name in \"" +
                    std::string(spec) + "\"";
            return std::nullopt;
        }
        names.emplace_back(name);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return ToolAllowlist(std::move(names));
}

std::string ToolAllowlist::unknown_tools_error(
    const std::vector<std::string>& available) const {
    std::vector<std::string> known = available;
    std::sort(known.begin(), known.end());
    std::vector<std::string> unknown;
    for (const auto& n : names_) {
        if (!std::binary_search(known.begin(), known.end(), n)) {
            unknown.push_back(n);
        }
    }
    if (unknown.empty()) return {};
    return "--tools names unknown tool(s): " + join(unknown) +
           "\nAvailable tools: " + join(known);
}

bool ToolAllowlist::allows(std::string_view name) const {
    return std::binary_search(names_.begin(), names_.end(), name);
}

nlohmann::json ToolAllowlist::refused_call_error(
    const std::string& name) const {
    return {{"code", -32602},
            {"message", "unknown tool \"" + name +
                            "\": this session serves only " + join(names_) +
                            " (lci mcp --tools)"}};
}

std::string ToolAllowlist::refused_call_frame(
    const nlohmann::json& request) const {
    // Notifications get no answer either way (dispatch_wire drops them), so
    // only a request with an id is answered locally.
    if (!request.is_object() || !request.contains("id")) return {};
    const auto method = request.find("method");
    if (method == request.end() || *method != "tools/call") return {};
    const auto params = request.find("params");
    if (params == request.end() || !params->is_object()) return {};
    const auto name = params->find("name");
    // A missing or mistyped name is the server's malformed-request error to
    // give, not a refusal.
    if (name == params->end() || !name->is_string()) return {};
    const auto& tool = name->get_ref<const std::string&>();
    if (allows(tool)) return {};
    return dump_json_lossy({{"jsonrpc", "2.0"},
                            {"id", request["id"]},
                            {"error", refused_call_error(tool)}});
}

std::string ToolAllowlist::filter_tools_list_frame(
    const std::string& response) const {
    // ordered_json keeps the server's emitted field order on re-dump.
    auto frame = nlohmann::ordered_json::parse(response, nullptr,
                                               /*allow_exceptions=*/false);
    if (!frame.is_object()) return response;
    const auto result = frame.find("result");
    if (result == frame.end() || !result->is_object()) return response;
    const auto tools = result->find("tools");
    if (tools == result->end() || !tools->is_array()) return response;

    nlohmann::ordered_json kept = nlohmann::ordered_json::array();
    for (auto& tool : *tools) {
        const auto name = tool.find("name");
        if (name != tool.end() && name->is_string() &&
            allows(name->get_ref<const std::string&>())) {
            kept.push_back(std::move(tool));
        }
    }
    *tools = std::move(kept);
    return frame.dump(-1, ' ', /*ensure_ascii=*/false,
                      nlohmann::ordered_json::error_handler_t::replace);
}

}  // namespace mcp
}  // namespace lci
