#include <lci/mcp/server.h>

namespace lci {
namespace mcp {

// Serialize with U+FFFD replacement for invalid UTF-8 instead of the default
// strict handler, which THROWS (type_error.316) on the first non-UTF-8 byte.
// See the header for the full rationale. Found by fuzz_get_context (byte 0x8A).
std::string dump_json_lossy(const nlohmann::json& data) {
    return data.dump(-1, ' ', /*ensure_ascii=*/false,
                     nlohmann::json::error_handler_t::replace);
}

ToolResult make_json_response(const nlohmann::json& data) {
    return {dump_json_lossy(data), false};
}

ToolResult make_error_response(const std::string& operation,
                               const std::string& message) {
    nlohmann::json data;
    data["success"] = false;
    data["error"] = message;
    data["operation"] = operation;
    return {dump_json_lossy(data), true};
}

}  // namespace mcp
}  // namespace lci
