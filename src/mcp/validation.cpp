// FIX-E: thin wire-format helpers only. Schema validation routes through
// nlohmann-json-schema-validator (handlers_core.cpp). See validation.h.
#include <lci/mcp/validation.h>
namespace lci {
namespace mcp {

std::string error_code_string(ValidationErrorCode code) {
    switch (code) {
        case ValidationErrorCode::kRequired:      return "REQUIRED";
        case ValidationErrorCode::kInvalid:       return "INVALID";
        case ValidationErrorCode::kTooLong:       return "TOO_LONG";
        case ValidationErrorCode::kTooShort:      return "TOO_SHORT";
        case ValidationErrorCode::kOutOfRange:    return "OUT_OF_RANGE";
        case ValidationErrorCode::kInvalidFormat: return "INVALID_FORMAT";
        case ValidationErrorCode::kInvalidEnum:   return "INVALID_ENUM";
        case ValidationErrorCode::kConflict:      return "CONFLICT";
    }
    return "UNKNOWN";
}

std::string ValidationError::to_string() const {
    return "validation error for field '" + field + "': " + message;
}

void ValidationResult::add_error(ValidationError err) {
    valid = false;
    errors.push_back(std::move(err));
}

std::string validation_summary(const ValidationResult& r) {
    if (r.valid) return "Validation passed";
    const auto& f = r.errors[0];
    if (r.errors.size() == 1)
        return "Validation failed: " + f.message + " (field: " + f.field +
               ", code: " + error_code_string(f.code) + ")";
    return "Validation failed with " + std::to_string(r.errors.size()) +
           " errors. First error: " + f.message + " (field: " + f.field + ")";
}

static std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\n\r");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\n\r") - b + 1);
}

static bool has_nonempty_string(const nlohmann::json& p, const char* k) {
    return p.contains(k) && p[k].is_string() &&
           !trim(p[k].get<std::string>()).empty();
}

std::optional<ValidationError> validate_search_business_logic(
    const nlohmann::json& params) {
    bool hp = has_nonempty_string(params, "pattern");
    bool hps = has_nonempty_string(params, "patterns");
    if (!hp && !hps)
        return ValidationError{"pattern", "pattern cannot be empty", nullptr,
            ValidationErrorCode::kRequired,
            {{"has_pattern", hp}, {"has_patterns", hps}}};
    return std::nullopt;
}

static nlohmann::json detail(const ValidationError& e) {
    return {{"error_code", error_code_string(e.code)}, {"field_name", e.field},
            {"error_message", e.message}, {"provided_value", e.value},
            {"help_context", e.context}};
}

nlohmann::json create_validation_error_response(
    const std::string& tool_name, const ValidationError& err,
    const nlohmann::json& extra_context) {
    nlohmann::json resp;
    resp["success"] = false;
    resp["error"] = {{"type", "validation_error"}, {"tool", tool_name},
        {"code", error_code_string(err.code)}, {"field", err.field},
        {"message", err.message}, {"value", err.value}, {"context", err.context}};
    resp["validation_details"] = detail(err);
    if (!extra_context.is_null() && extra_context.is_object())
        for (auto& [k, v] : extra_context.items()) resp["error"][k] = v;
    return resp;
}

nlohmann::json create_multi_validation_error_response(
    const std::string& tool_name, const std::vector<ValidationError>& errors,
    const nlohmann::json& extra_context) {
    nlohmann::json details = nlohmann::json::array();
    for (const auto& e : errors) details.push_back(detail(e));
    nlohmann::json resp;
    resp["success"] = false;
    resp["error"] = {{"type", "multiple_validation_errors"}, {"tool", tool_name},
        {"message", "Request failed with " + std::to_string(errors.size()) +
                        " validation errors"},
        {"count", errors.size()}};
    resp["validation_errors"] = details;
    if (!extra_context.is_null() && extra_context.is_object())
        resp["context"] = extra_context;
    return resp;
}
}  // namespace mcp
}  // namespace lci
