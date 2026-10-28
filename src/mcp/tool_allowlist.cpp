#include <lci/mcp/tool_allowlist.h>

#include <algorithm>
#include <cctype>

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

// The registered tool an info call or overview entry names: lower-cased, with
// the historic "files" short name mapped to find_files (as handle_info does).
std::string info_tool_name(std::string_view name) {
    std::string lowered(name);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lowered == "files" ? "find_files" : lowered;
}

constexpr std::string_view kInfoTool = "info";

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

nlohmann::json ToolAllowlist::call_refusal(
    const nlohmann::json& params) const {
    // A missing or mistyped name or argument is the server's malformed-
    // request error to give, not a refusal.
    if (!params.is_object()) return nullptr;
    const auto name = params.find("name");
    if (name == params.end() || !name->is_string()) return nullptr;
    const auto& tool = name->get_ref<const std::string&>();
    if (!allows(tool)) return refused_call_error(tool);
    if (tool != kInfoTool) return nullptr;

    const auto arguments = params.find("arguments");
    if (arguments == params.end() || !arguments->is_object()) return nullptr;
    const auto subject = arguments->find("tool");
    if (subject == arguments->end() || !subject->is_string()) return nullptr;
    const auto asked =
        info_tool_name(subject->get_ref<const std::string&>());
    // "" is the overview and "version" is server info; neither names a tool.
    if (asked.empty() || asked == "version" || allows(asked)) return nullptr;
    return refused_call_error(subject->get<std::string>());
}

void ToolAllowlist::narrow_call_response(const std::string& tool_name,
                                         nlohmann::json& response) const {
    if (tool_name != kInfoTool) return;
    // Only the overview carries available_tools; help for one tool and
    // version info pass through untouched.
    if (!response.contains("result") || !response["result"].is_object()) {
        return;
    }
    auto& result = response["result"];
    if (!result.contains("content") || !result["content"].is_array() ||
        result["content"].empty()) {
        return;
    }
    auto& item = result["content"][0];
    if (!item.contains("text") || !item["text"].is_string()) return;
    auto overview = nlohmann::json::parse(item["text"].get<std::string>(),
                                          nullptr, /*allow_exceptions=*/false);
    if (!overview.is_object() || !overview.contains("available_tools") ||
        !overview["available_tools"].is_array()) {
        return;
    }
    nlohmann::json kept = nlohmann::json::array();
    for (auto& entry : overview["available_tools"]) {
        if (!entry.is_string()) continue;
        const auto& line = entry.get_ref<const std::string&>();
        // Entries read "<name> - ..." or "info [tool] - ...".
        if (allows(info_tool_name(line.substr(0, line.find(' '))))) {
            kept.push_back(std::move(entry));
        }
    }
    overview["available_tools"] = std::move(kept);
    item["text"] = dump_json_lossy(overview);
}

std::string ToolAllowlist::refused_call_frame(
    const nlohmann::json& request) const {
    // Notifications get no answer either way (dispatch_wire drops them), so
    // only a request with an id is answered locally.
    if (!request.is_object() || !request.contains("id")) return {};
    const auto method = request.find("method");
    if (method == request.end() || *method != "tools/call") return {};
    const auto params = request.find("params");
    if (params == request.end()) return {};
    auto refusal = call_refusal(*params);
    if (refusal.is_null()) return {};
    return dump_json_lossy({{"jsonrpc", "2.0"},
                            {"id", request["id"]},
                            {"error", std::move(refusal)}});
}

std::string ToolAllowlist::narrow_call_response_frame(
    const std::string& tool_name, const std::string& response) const {
    if (tool_name != kInfoTool) return response;
    auto frame = nlohmann::json::parse(response, nullptr,
                                       /*allow_exceptions=*/false);
    if (!frame.is_object()) return response;
    narrow_call_response(tool_name, frame);
    return dump_json_lossy(frame);
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
