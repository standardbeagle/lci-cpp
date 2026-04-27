#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <lci/scope.h>
#include <lci/types.h>

namespace lci {

/// Kind of reference relationship between symbols.
enum class ReferenceType : uint8_t {
    Import = 0,
    Call,
    Inheritance,
    Assignment,
    Declaration,
    Parameter,
    Return,
    TypeAnnotation,
    Implements,
    Extends,
    Usage,
};

/// Returns the string name for a ReferenceType value.
constexpr std::string_view to_string(ReferenceType rt) {
    switch (rt) {
        case ReferenceType::Import: return "import";
        case ReferenceType::Call: return "call";
        case ReferenceType::Inheritance: return "inheritance";
        case ReferenceType::Assignment: return "assignment";
        case ReferenceType::Declaration: return "declaration";
        case ReferenceType::Parameter: return "parameter";
        case ReferenceType::Return: return "return";
        case ReferenceType::TypeAnnotation: return "type_annotation";
        case ReferenceType::Implements: return "implements";
        case ReferenceType::Extends: return "extends";
        case ReferenceType::Usage: return "usage";
    }
    return "unknown";
}

/// Coupling strength of a reference.
enum class RefStrength : uint8_t {
    Tight = 0,
    Loose,
    Transitive,
};

/// Returns the string name for a RefStrength value.
constexpr std::string_view to_string(RefStrength rs) {
    switch (rs) {
        case RefStrength::Tight: return "tight";
        case RefStrength::Loose: return "loose";
        case RefStrength::Transitive: return "transitive";
    }
    return "unknown";
}

/// A relationship between symbols (call, import, inheritance, etc.).
struct Reference {
    uint64_t id{};
    SymbolID source_symbol{};
    SymbolID target_symbol{};
    FileID file_id{};
    int line{};
    int column{};
    ReferenceType type{};
    std::vector<ScopeInfo> scope_context;
    RefStrength strength{};
    std::string referenced_name;
    std::string quality;
    bool ambiguous{};
    std::vector<std::string> candidates;
    std::string failure_reason;
};

/// Breakdown of references by coupling strength.
struct RefStrengthStats {
    int tight{};
    int loose{};
    int transitive{};
};

/// Reference count statistics.
struct RefCount {
    int incoming_count{};
    int outgoing_count{};
    std::vector<FileID> incoming_files;
    std::vector<FileID> outgoing_files;
    RefStrengthStats strength;
};

/// Reference statistics at multiple scope levels.
struct RefStats {
    RefCount folder_level;
    RefCount file_level;
    RefCount class_level;
    RefCount function_level;
    RefCount variable_level;
    RefCount total;
};

/// An import statement in a source file.
struct Import {
    std::string path;
    FileID file_id{};
    int line{};
};

}  // namespace lci
