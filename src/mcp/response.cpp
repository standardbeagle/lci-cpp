#include <lci/mcp/server.h>

namespace lci {
namespace mcp {

ToolResult make_json_response(const nlohmann::json& data) {
    return {data.dump(), false};
}

ToolResult make_error_response(const std::string& operation,
                               const std::string& message) {
    nlohmann::json data;
    data["success"] = false;
    data["error"] = message;
    data["operation"] = operation;
    return {data.dump(), true};
}

}  // namespace mcp
}  // namespace lci
