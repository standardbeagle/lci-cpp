#include <lci/mcp/validation.h>

#include <algorithm>
#include <cctype>

namespace lci {
namespace mcp {

// -- error_code_string --------------------------------------------------------

std::string error_code_string(ValidationErrorCode code) {
    switch (code) {
        case ValidationErrorCode::kRequired:      return "REQUIRED";
        case ValidationErrorCode::kInvalid:        return "INVALID";
        case ValidationErrorCode::kTooLong:        return "TOO_LONG";
        case ValidationErrorCode::kTooShort:       return "TOO_SHORT";
        case ValidationErrorCode::kOutOfRange:     return "OUT_OF_RANGE";
        case ValidationErrorCode::kInvalidFormat:  return "INVALID_FORMAT";
        case ValidationErrorCode::kInvalidEnum:    return "INVALID_ENUM";
        case ValidationErrorCode::kConflict:       return "CONFLICT";
    }
    return "UNKNOWN";
}

// -- ValidationError ----------------------------------------------------------

std::string ValidationError::to_string() const {
    return "validation error for field '" + field + "': " + message;
}

// -- ValidationResult ---------------------------------------------------------

void ValidationResult::add_error(ValidationError err) {
    valid = false;
    errors.push_back(std::move(err));
}

// -- validation_summary -------------------------------------------------------

std::string validation_summary(const ValidationResult& result) {
    if (result.valid) return "Validation passed";

    if (result.errors.size() == 1) {
        const auto& e = result.errors[0];
        return "Validation failed: " + e.message + " (field: " + e.field +
               ", code: " + error_code_string(e.code) + ")";
    }

    return "Validation failed with " +
           std::to_string(result.errors.size()) +
           " errors. First error: " + result.errors[0].message +
           " (field: " + result.errors[0].field + ")";
}

// -- Helper: trim whitespace --------------------------------------------------

static std::string trim(const std::string& s) {
    auto begin = s.find_first_not_of(" \t\n\r");
    if (begin == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(begin, end - begin + 1);
}

// -- Built-in rules -----------------------------------------------------------

ValidationRule required_rule() {
    return {"required", [](const nlohmann::json& value)
                -> std::optional<ValidationError> {
        if (value.is_null()) {
            return ValidationError{"", "field is required",
                                   nullptr, ValidationErrorCode::kRequired};
        }
        if (value.is_string()) {
            if (trim(value.get<std::string>()).empty()) {
                return ValidationError{"", "field cannot be empty",
                                       nullptr, ValidationErrorCode::kRequired};
            }
        }
        if (value.is_array() && value.empty()) {
            return ValidationError{"", "field cannot be empty array",
                                   nullptr, ValidationErrorCode::kRequired};
        }
        return std::nullopt;
    }};
}

ValidationRule min_length_rule(int min_length) {
    return {"min_length", [min_length](const nlohmann::json& value)
                -> std::optional<ValidationError> {
        if (value.is_null()) return std::nullopt;

        if (value.is_string()) {
            auto s = value.get<std::string>();
            int len = static_cast<int>(s.size());
            if (len < min_length) {
                return ValidationError{
                    "", "must have at least " + std::to_string(min_length) +
                            " characters",
                    value, ValidationErrorCode::kTooShort,
                    {{"min_length", min_length}, {"actual_length", len}}};
            }
        }
        if (value.is_array()) {
            int len = static_cast<int>(value.size());
            if (len < min_length) {
                return ValidationError{
                    "",
                    "minimum array length is " + std::to_string(min_length),
                    value, ValidationErrorCode::kTooShort,
                    {{"min_length", min_length}, {"actual_length", len}}};
            }
        }
        return std::nullopt;
    }};
}

ValidationRule max_length_rule(int max_length) {
    return {"max_length", [max_length](const nlohmann::json& value)
                -> std::optional<ValidationError> {
        if (value.is_null()) return std::nullopt;

        if (value.is_string()) {
            auto s = value.get<std::string>();
            int len = static_cast<int>(s.size());
            if (len > max_length) {
                return ValidationError{
                    "",
                    "is too long (max " + std::to_string(max_length) +
                        " characters)",
                    value, ValidationErrorCode::kTooLong,
                    {{"max_length", max_length}, {"actual_length", len}}};
            }
        }
        if (value.is_array()) {
            int len = static_cast<int>(value.size());
            if (len > max_length) {
                return ValidationError{
                    "",
                    "maximum array length is " + std::to_string(max_length),
                    value, ValidationErrorCode::kTooLong,
                    {{"max_length", max_length}, {"actual_length", len}}};
            }
        }
        return std::nullopt;
    }};
}

ValidationRule range_rule(int min_val, int max_val) {
    return {"range", [min_val, max_val](const nlohmann::json& value)
                -> std::optional<ValidationError> {
        if (value.is_null()) return std::nullopt;

        int64_t int_value = 0;
        if (value.is_number_integer()) {
            int_value = value.get<int64_t>();
        } else if (value.is_number_float()) {
            double d = value.get<double>();
            int_value = static_cast<int64_t>(d);
        } else {
            return ValidationError{"", "value must be a number",
                                   value, ValidationErrorCode::kInvalidFormat};
        }

        if (int_value < min_val || int_value > max_val) {
            return ValidationError{
                "",
                "value must be between " + std::to_string(min_val) + " and " +
                    std::to_string(max_val),
                value, ValidationErrorCode::kOutOfRange,
                {{"min", min_val}, {"max", max_val}}};
        }
        return std::nullopt;
    }};
}

ValidationRule enum_rule(const std::vector<std::string>& valid_values) {
    return {"enum", [valid_values](const nlohmann::json& value)
                -> std::optional<ValidationError> {
        if (value.is_null()) return std::nullopt;

        if (!value.is_string()) {
            return ValidationError{"", "value must be a string",
                                   value, ValidationErrorCode::kInvalidFormat};
        }

        auto s = value.get<std::string>();
        for (const auto& v : valid_values) {
            if (s == v) return std::nullopt;
        }

        std::string joined;
        for (size_t i = 0; i < valid_values.size(); ++i) {
            if (i > 0) joined += ", ";
            joined += valid_values[i];
        }

        return ValidationError{
            "", "value must be one of: " + joined,
            s, ValidationErrorCode::kInvalidEnum,
            {{"valid_values", valid_values}}};
    }};
}

ValidationRule regex_rule(const std::string& pattern,
                          const std::string& message) {
    auto re = std::regex(pattern);
    auto msg = message.empty() ? ("value must match pattern: " + pattern)
                               : message;

    return {"regex", [re, msg, pattern](const nlohmann::json& value)
                -> std::optional<ValidationError> {
        if (value.is_null()) return std::nullopt;

        if (!value.is_string()) {
            return ValidationError{"", "value must be a string",
                                   value, ValidationErrorCode::kInvalidFormat};
        }

        auto s = value.get<std::string>();
        if (!std::regex_search(s, re)) {
            return ValidationError{
                "", msg, s, ValidationErrorCode::kInvalidFormat,
                {{"pattern", pattern}}};
        }
        return std::nullopt;
    }};
}

// -- RequestValidator ---------------------------------------------------------

void RequestValidator::add_rule(const std::string& field,
                                ValidationRule rule) {
    rules_[field].push_back(std::move(rule));
}

ValidationResult RequestValidator::validate(
    const nlohmann::json& params) const {
    ValidationResult result;

    if (!params.is_object()) {
        result.add_error({"request", "request must be a JSON object",
                          nullptr, ValidationErrorCode::kInvalid});
        return result;
    }

    for (const auto& [field, rules] : rules_) {
        nlohmann::json value = nullptr;
        if (params.contains(field)) {
            value = params[field];
        }

        for (const auto& rule : rules) {
            auto err = rule.validate(value);
            if (err) {
                if (err->field.empty()) {
                    err->field = field;
                }
                result.add_error(std::move(*err));
            }
        }
    }

    return result;
}

// -- Validator factories ------------------------------------------------------

RequestValidator create_search_validator() {
    RequestValidator v;

    v.add_rule("pattern", {"pattern_length",
        [](const nlohmann::json& value) -> std::optional<ValidationError> {
            if (value.is_null()) return std::nullopt;
            if (!value.is_string()) {
                return ValidationError{"pattern", "pattern must be a string",
                                       value, ValidationErrorCode::kInvalidFormat};
            }
            auto s = value.get<std::string>();
            if (s.empty()) {
                return ValidationError{"pattern", "pattern cannot be empty",
                                       value, ValidationErrorCode::kTooShort,
                                       {{"min_length", 1}, {"actual_length", 0}}};
            }
            if (static_cast<int>(s.size()) > 100000) {
                return ValidationError{"pattern", "pattern is too long (max 100000 characters)",
                                       value, ValidationErrorCode::kTooLong,
                                       {{"max_length", 100000},
                                        {"actual_length", static_cast<int>(s.size())}}};
            }
            return std::nullopt;
        }});

    v.add_rule("max", {"max_non_negative",
        [](const nlohmann::json& value) -> std::optional<ValidationError> {
            if (value.is_null()) return std::nullopt;
            if (!value.is_number()) {
                return ValidationError{"max", "max must be a number",
                                       value, ValidationErrorCode::kInvalidFormat};
            }
            double d = value.get<double>();
            if (d < 0) {
                return ValidationError{"max", "max cannot be negative",
                                       value, ValidationErrorCode::kOutOfRange};
            }
            return std::nullopt;
        }});

    v.add_rule("max_per_file", {"max_per_file_non_negative",
        [](const nlohmann::json& value) -> std::optional<ValidationError> {
            if (value.is_null()) return std::nullopt;
            if (!value.is_number()) {
                return ValidationError{"max_per_file", "max_per_file must be a number",
                                       value, ValidationErrorCode::kInvalidFormat};
            }
            double d = value.get<double>();
            if (d < 0) {
                return ValidationError{"max_per_file", "max_per_file cannot be negative",
                                       value, ValidationErrorCode::kOutOfRange};
            }
            return std::nullopt;
        }});

    return v;
}

RequestValidator create_symbol_validator() {
    RequestValidator v;

    v.add_rule("symbol", {"symbol_required",
        [](const nlohmann::json& value) -> std::optional<ValidationError> {
            if (value.is_null()) {
                return ValidationError{"symbol", "symbol cannot be empty",
                                       nullptr, ValidationErrorCode::kRequired};
            }
            if (!value.is_string()) {
                return ValidationError{"symbol", "symbol must be a string",
                                       value, ValidationErrorCode::kInvalidFormat};
            }
            if (trim(value.get<std::string>()).empty()) {
                return ValidationError{"symbol", "symbol cannot be empty",
                                       nullptr, ValidationErrorCode::kRequired};
            }
            return std::nullopt;
        }});
    v.add_rule("symbol", min_length_rule(1));
    v.add_rule("symbol", max_length_rule(100000));

    return v;
}

RequestValidator create_tree_validator() {
    RequestValidator v;
    v.add_rule("function", required_rule());
    v.add_rule("function", min_length_rule(1));
    v.add_rule("function", max_length_rule(500));
    v.add_rule("max_depth", range_rule(0, 50));
    return v;
}

// -- Business logic validators ------------------------------------------------

std::optional<ValidationError> validate_search_business_logic(
    const nlohmann::json& params) {
    bool has_pattern = params.contains("pattern") &&
                       params["pattern"].is_string() &&
                       !trim(params["pattern"].get<std::string>()).empty();
    bool has_patterns = params.contains("patterns") &&
                        params["patterns"].is_string() &&
                        !trim(params["patterns"].get<std::string>()).empty();

    if (!has_pattern && !has_patterns) {
        return ValidationError{
            "pattern", "pattern cannot be empty",
            nullptr, ValidationErrorCode::kRequired,
            {{"has_pattern", has_pattern}, {"has_patterns", has_patterns}}};
    }
    return std::nullopt;
}

std::optional<ValidationError> validate_object_context_business_logic(
    const nlohmann::json& params) {
    bool has_id = params.contains("id") && params["id"].is_string() &&
                  !trim(params["id"].get<std::string>()).empty();
    bool has_name = params.contains("name") && params["name"].is_string() &&
                    !trim(params["name"].get<std::string>()).empty();

    if (!has_id && !has_name) {
        return ValidationError{
            "id",
            "missing required 'id' parameter with object ID from search results",
            nullptr, ValidationErrorCode::kRequired,
            {{"example", "{\"id\": \"VE\"} or {\"id\": \"VE,tG,Ab\"} for multiple"},
             {"workflow", "1. Search: {\"pattern\": \"X\"} -> 2. Find o=XX -> 3. Context: {\"id\": \"XX\"}"}}};
    }

    if (has_id && has_name) {
        return ValidationError{
            "id,name",
            "parameter conflict: use either 'id' OR 'name', not both",
            nullptr, ValidationErrorCode::kConflict,
            {{"recommendation",
              "Prefer 'id' parameter with object IDs from search results"}}};
    }

    return std::nullopt;
}

bool is_valid_object_id(const std::string& object_id) {
    if (object_id.empty()) return false;
    for (char c : object_id) {
        bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '_';
        if (!valid) return false;
    }
    return true;
}

// -- Structured error responses -----------------------------------------------

nlohmann::json create_validation_error_response(
    const std::string& tool_name, const ValidationError& error,
    const nlohmann::json& extra_context) {
    nlohmann::json resp;
    resp["success"] = false;
    resp["error"] = {
        {"type", "validation_error"},
        {"tool", tool_name},
        {"code", error_code_string(error.code)},
        {"field", error.field},
        {"message", error.message},
        {"value", error.value},
        {"context", error.context},
    };
    resp["validation_details"] = {
        {"error_code", error_code_string(error.code)},
        {"field_name", error.field},
        {"error_message", error.message},
        {"provided_value", error.value},
        {"help_context", error.context},
    };

    if (!extra_context.is_null() && extra_context.is_object()) {
        for (auto& [k, v] : extra_context.items()) {
            resp["error"][k] = v;
        }
    }

    return resp;
}

nlohmann::json create_multi_validation_error_response(
    const std::string& tool_name,
    const std::vector<ValidationError>& errors,
    const nlohmann::json& extra_context) {
    nlohmann::json details = nlohmann::json::array();
    for (const auto& e : errors) {
        details.push_back({
            {"error_code", error_code_string(e.code)},
            {"field_name", e.field},
            {"error_message", e.message},
            {"provided_value", e.value},
            {"help_context", e.context},
        });
    }

    nlohmann::json resp;
    resp["success"] = false;
    resp["error"] = {
        {"type", "multiple_validation_errors"},
        {"tool", tool_name},
        {"message", "Request failed with " + std::to_string(errors.size()) +
                        " validation errors"},
        {"count", errors.size()},
    };
    resp["validation_errors"] = details;

    if (!extra_context.is_null() && extra_context.is_object()) {
        resp["context"] = extra_context;
    }

    return resp;
}

}  // namespace mcp
}  // namespace lci
