#include <lci/error.h>

#include <string>

namespace lci {

std::string Error::to_string() const {
    std::string type_name{lci::to_string(type)};

    switch (type) {
        case ErrorType::Parse:
            return "parse error at " + file_path + ":" +
                   std::to_string(line) + ":" + std::to_string(column) +
                   " (near token \"" + token + "\"): " + message;
        case ErrorType::Config:
            return "config error for field " + config_field +
                   " (value " + config_value + "): " + message;
        case ErrorType::Search:
            return "search failed for pattern \"" + operation + "\": " + message;
        case ErrorType::FileNotFound:
        case ErrorType::FileTooLarge:
        case ErrorType::Permission:
            return "file " + operation + " failed for " + file_path + ": " + message;
        case ErrorType::Indexing:
        case ErrorType::Internal:
            if (!file_path.empty()) {
                return type_name + " " + operation + " failed for " +
                       file_path + ": " + message;
            }
            return type_name + " " + operation + " failed: " + message;
    }
    return type_name + ": " + message;
}

std::string MultiError::to_string() const {
    if (errors.empty()) return "no errors";
    if (errors.size() == 1) return errors[0].to_string();
    return std::to_string(errors.size()) + " errors occurred";
}

}  // namespace lci
