#pragma once

#include <optional>
#include <string>
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

// -- Business logic validators ------------------------------------------------
//
// FIX-E (sGNoQG6QcTGB): RequestValidator + ValidationRule + all *_rule() and
// create_*_validator() factories removed. Schema validation now lives in
// share/lci/mcp-schemas/*.json via nlohmann-json-schema-validator (see
// handlers_core.cpp SearchSchemaErrorCollector). Only the wire-format error
// shape (ValidationError, error_code_string, create_*_response) and the
// search business-logic checker remain — the latter is still called from
// handle_search for the missing-pattern path; it will retire when search
// schema fully replaces it in a follow-up.

/// Validates search business logic (pattern/patterns mutual requirement).
std::optional<ValidationError> validate_search_business_logic(
    const nlohmann::json& params);

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
