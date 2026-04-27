#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <lci/types.h>

namespace lci {

/// Classification of errors in the indexing system.
enum class ErrorType : uint8_t {
    Indexing = 0,
    Parse,
    Search,
    FileNotFound,
    FileTooLarge,
    Permission,
    Config,
    Internal,
};

/// Returns the string name for an ErrorType value.
constexpr std::string_view to_string(ErrorType et) {
    switch (et) {
        case ErrorType::Indexing: return "indexing";
        case ErrorType::Parse: return "parse";
        case ErrorType::Search: return "search";
        case ErrorType::FileNotFound: return "file_not_found";
        case ErrorType::FileTooLarge: return "file_too_large";
        case ErrorType::Permission: return "permission";
        case ErrorType::Config: return "config";
        case ErrorType::Internal: return "internal";
    }
    return "unknown";
}

/// Error with context information for the indexing system.
/// Carries the error type, a human-readable message, and optional
/// file/operation context for diagnostics.
struct Error {
    ErrorType type{};
    std::string message;
    std::string file_path;
    FileID file_id{};
    std::string operation;
    bool recoverable{};

    /// Source location for parse errors.
    int line{};
    int column{};
    std::string token;

    /// Config error context.
    std::string config_field;
    std::string config_value;

    /// Formats the error as a human-readable string.
    std::string to_string() const;
};

/// Creates an indexing error with operation context.
inline Error make_indexing_error(std::string_view operation, std::string_view message) {
    Error e;
    e.type = ErrorType::Indexing;
    e.operation = std::string(operation);
    e.message = std::string(message);
    return e;
}

/// Creates a parse error with source location.
inline Error make_parse_error(FileID file_id, std::string_view path,
                              int line, int column,
                              std::string_view token, std::string_view message) {
    Error e;
    e.type = ErrorType::Parse;
    e.file_id = file_id;
    e.file_path = std::string(path);
    e.line = line;
    e.column = column;
    e.token = std::string(token);
    e.message = std::string(message);
    return e;
}

/// Creates a search error with pattern context.
inline Error make_search_error(std::string_view pattern, std::string_view message) {
    Error e;
    e.type = ErrorType::Search;
    e.message = std::string(message);
    e.operation = std::string(pattern);
    return e;
}

/// Creates a file error, auto-detecting permission errors.
inline Error make_file_error(std::string_view operation, std::string_view path,
                             std::string_view message) {
    Error e;
    e.type = (message == "permission denied" || message == "access denied")
                 ? ErrorType::Permission
                 : ErrorType::FileNotFound;
    e.operation = std::string(operation);
    e.file_path = std::string(path);
    e.message = std::string(message);
    return e;
}

/// Creates a configuration error.
inline Error make_config_error(std::string_view field, std::string_view value,
                               std::string_view message) {
    Error e;
    e.type = ErrorType::Config;
    e.config_field = std::string(field);
    e.config_value = std::string(value);
    e.message = std::string(message);
    return e;
}

/// Aggregate of multiple errors.
struct MultiError {
    std::vector<Error> errors;

    /// Formats all errors as a single string.
    std::string to_string() const;
};

/// Generic result type holding either a value or an Error.
/// Follows the same pattern as DecodeResult but with Error instead of CodecErrc.
template <typename T>
class Result {
  public:
    Result(T value) : data_(std::move(value)) {}   // NOLINT(implicit)
    Result(Error err) : data_(std::move(err)) {}    // NOLINT(implicit)

    bool has_value() const { return std::holds_alternative<T>(data_); }
    explicit operator bool() const { return has_value(); }

    const T& value() const { return std::get<T>(data_); }
    T& value() { return std::get<T>(data_); }
    const T& operator*() const { return value(); }
    T& operator*() { return value(); }

    const Error& error() const { return std::get<Error>(data_); }
    Error& error() { return std::get<Error>(data_); }

  private:
    std::variant<T, Error> data_;
};

}  // namespace lci
