#pragma once

#include <cstdint>
#include <string_view>

namespace lci {

/// Strong type alias for file identifiers.
using FileID = uint32_t;

/// Strong type alias for symbol identifiers.
using SymbolID = uint64_t;

/// Classification of symbols found during AST parsing.
/// Matches Go's SymbolType enum (26 variants).
enum class SymbolType : uint8_t {
    Function = 0,
    Class,
    Method,
    Variable,
    Constant,
    Interface,
    Type,
    Struct,
    Module,
    Namespace,
    Property,
    Event,
    Delegate,
    Enum,
    Record,
    Operator,
    Indexer,
    Object,
    Companion,
    Extension,
    Annotation,
    Field,
    EnumMember,
    Trait,
    Impl,
    Constructor,
};

/// Total number of SymbolType variants.
inline constexpr int kSymbolTypeCount = 26;

/// Returns the string name for a SymbolType value.
constexpr std::string_view to_string(SymbolType st) {
    switch (st) {
        case SymbolType::Function: return "function";
        case SymbolType::Class: return "class";
        case SymbolType::Method: return "method";
        case SymbolType::Variable: return "variable";
        case SymbolType::Constant: return "constant";
        case SymbolType::Interface: return "interface";
        case SymbolType::Type: return "type";
        case SymbolType::Struct: return "struct";
        case SymbolType::Module: return "module";
        case SymbolType::Namespace: return "namespace";
        case SymbolType::Property: return "property";
        case SymbolType::Event: return "event";
        case SymbolType::Delegate: return "delegate";
        case SymbolType::Enum: return "enum";
        case SymbolType::Record: return "record";
        case SymbolType::Operator: return "operator";
        case SymbolType::Indexer: return "indexer";
        case SymbolType::Object: return "object";
        case SymbolType::Companion: return "companion";
        case SymbolType::Extension: return "extension";
        case SymbolType::Annotation: return "annotation";
        case SymbolType::Field: return "field";
        case SymbolType::EnumMember: return "enum_member";
        case SymbolType::Trait: return "trait";
        case SymbolType::Impl: return "impl";
        case SymbolType::Constructor: return "constructor";
    }
    return "unknown";
}

/// Context-altering attribute types that affect code behavior.
enum class ContextAttributeType : uint8_t {
    Directive = 0,
    Unsafe,
    Lock,
    Decorator,
    Pragma,
    Iterator,
    Async,
    Volatile,
    Deprecated,
    Experimental,
    Pure,
    NoThrow,
    SideEffect,
    Recursive,
    Exported,
    Inline,
    Virtual,
    Abstract,
    Static,
    Final,
    Const,
    Generator,
    Coroutine,
};

/// Returns the string name for a ContextAttributeType value.
constexpr std::string_view to_string(ContextAttributeType cat) {
    switch (cat) {
        case ContextAttributeType::Directive: return "directive";
        case ContextAttributeType::Unsafe: return "unsafe";
        case ContextAttributeType::Lock: return "lock";
        case ContextAttributeType::Decorator: return "decorator";
        case ContextAttributeType::Pragma: return "pragma";
        case ContextAttributeType::Iterator: return "iterator";
        case ContextAttributeType::Async: return "async";
        case ContextAttributeType::Volatile: return "volatile";
        case ContextAttributeType::Deprecated: return "deprecated";
        case ContextAttributeType::Experimental: return "experimental";
        case ContextAttributeType::Pure: return "pure";
        case ContextAttributeType::NoThrow: return "nothrow";
        case ContextAttributeType::SideEffect: return "side_effect";
        case ContextAttributeType::Recursive: return "recursive";
        case ContextAttributeType::Exported: return "exported";
        case ContextAttributeType::Inline: return "inline";
        case ContextAttributeType::Virtual: return "virtual";
        case ContextAttributeType::Abstract: return "abstract";
        case ContextAttributeType::Static: return "static";
        case ContextAttributeType::Final: return "final";
        case ContextAttributeType::Const: return "const";
        case ContextAttributeType::Generator: return "generator";
        case ContextAttributeType::Coroutine: return "coroutine";
    }
    return "unknown";
}

/// Visibility/export status of a symbol.
enum class SymbolVisibility : uint8_t {
    Default = 0,
    Public,
    Private,
    Protected,
    Internal,
    Package,
};

/// Classification of variables by scope and role.
enum class VariableType : uint8_t {
    Global = 0,
    Local,
    Parameter,
    Field,
    Member,
    Constant,
};

/// Returns the string name for a VariableType value.
constexpr std::string_view to_string(VariableType vt) {
    switch (vt) {
        case VariableType::Global: return "global";
        case VariableType::Local: return "local";
        case VariableType::Parameter: return "parameter";
        case VariableType::Field: return "field";
        case VariableType::Member: return "member";
        case VariableType::Constant: return "constant";
    }
    return "unknown";
}

/// A context-altering attribute on a symbol or scope.
struct ContextAttribute {
    ContextAttributeType type{};
    std::string value;
    int line{};
};

/// A generic type parameter (e.g., T in Foo<T>).
struct TypeParameter {
    std::string name;
    std::string constraint;
};

/// Bitfield constants for VariableFlags (uint8_t).
namespace variable_flags {
inline constexpr uint8_t kConst = 1 << 0;
inline constexpr uint8_t kStatic = 1 << 1;
inline constexpr uint8_t kPointer = 1 << 2;
inline constexpr uint8_t kArray = 1 << 3;
inline constexpr uint8_t kChannel = 1 << 4;
inline constexpr uint8_t kInterface = 1 << 5;
}  // namespace variable_flags

/// Bitfield constants for FunctionFlags (uint8_t).
namespace function_flags {
inline constexpr uint8_t kAsync = 1 << 0;
inline constexpr uint8_t kGenerator = 1 << 1;
inline constexpr uint8_t kMethod = 1 << 2;
inline constexpr uint8_t kVariadic = 1 << 3;
}  // namespace function_flags

}  // namespace lci
