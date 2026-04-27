#pragma once

#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace lci {
namespace mcp {

// -- Validation error codes ---------------------------------------------------

enum class ValidationErrorCode {
    kRequired,
    kInvalid,
    kTooLong,
    kTooShort,
    kOutOfRange,
    kInvalidFormat,
    kInvalidEnum,
    kConflict,
};

/// Returns the string representation of a validation error code.
std::string error_code_string(ValidationErrorCode code);

// -- ValidationError ----------------------------------------------------------

/// Represents a single parameter validation error.
struct ValidationError {
    std::string field;
    std::string message;
    nlohmann::json value;
    ValidationErrorCode code{ValidationErrorCode::kInvalid};
    nlohmann::json context;

    /// Returns a human-readable error string.
    std::string to_string() const;
};

// -- ValidationResult ---------------------------------------------------------

/// Aggregates the outcome of validating a request.
struct ValidationResult {
    bool valid{true};
    std::vector<ValidationError> errors;

    void add_error(ValidationError err);
};

/// Returns a human-readable summary of a validation result.
std::string validation_summary(const ValidationResult& result);

// -- ValidationRule -----------------------------------------------------------

/// A named validation rule that checks a JSON value.
struct ValidationRule {
    std::string name;
    std::function<std::optional<ValidationError>(const nlohmann::json& value)>
        validate;
};

// -- Built-in rules -----------------------------------------------------------

/// Field is required (non-null, non-empty string, non-empty array).
ValidationRule required_rule();

/// String minimum length.
ValidationRule min_length_rule(int min_length);

/// String maximum length.
ValidationRule max_length_rule(int max_length);

/// Integer range [min, max].
ValidationRule range_rule(int min_val, int max_val);

/// Value must be one of the given strings.
ValidationRule enum_rule(const std::vector<std::string>& valid_values);

/// Value must match a regex pattern.
ValidationRule regex_rule(const std::string& pattern,
                          const std::string& message = "");

// -- RequestValidator ---------------------------------------------------------

/// Validates JSON request parameters against registered rules.
class RequestValidator {
  public:
    /// Adds a validation rule for a named field.
    void add_rule(const std::string& field, ValidationRule rule);

    /// Validates all fields in the given JSON object.
    ValidationResult validate(const nlohmann::json& params) const;

  private:
    std::unordered_map<std::string, std::vector<ValidationRule>> rules_;
};

// -- Validator factories ------------------------------------------------------

/// Creates a validator for search requests.
RequestValidator create_search_validator();

/// Creates a validator for symbol requests.
RequestValidator create_symbol_validator();

/// Creates a validator for tree requests.
RequestValidator create_tree_validator();

// -- Business logic validators ------------------------------------------------

/// Validates search business logic (pattern/patterns mutual requirement).
std::optional<ValidationError> validate_search_business_logic(
    const nlohmann::json& params);

/// Validates object context business logic (id/name mutual exclusion).
std::optional<ValidationError> validate_object_context_business_logic(
    const nlohmann::json& params);

/// Checks whether an object ID follows the expected base-63 pattern.
bool is_valid_object_id(const std::string& object_id);

// -- Structured error responses -----------------------------------------------

/// Creates a JSON validation error response suitable for MCP.
nlohmann::json create_validation_error_response(
    const std::string& tool_name, const ValidationError& error,
    const nlohmann::json& extra_context = nullptr);

/// Creates a JSON response for multiple validation errors.
nlohmann::json create_multi_validation_error_response(
    const std::string& tool_name,
    const std::vector<ValidationError>& errors,
    const nlohmann::json& extra_context = nullptr);

}  // namespace mcp
}  // namespace lci
