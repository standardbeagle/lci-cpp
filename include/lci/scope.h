#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <lci/types.h>

namespace lci {

/// Type of a block boundary in source code.
enum class BlockType : uint8_t {
    Function = 0,
    Class,
    Method,
    Interface,
    Struct,
    Variable,
    Block,
    Enum,
    Trait,
    Impl,
    Module,
    Namespace,
    Constructor,
    Other,
};

/// Returns the string name for a BlockType value.
constexpr std::string_view to_string(BlockType bt) {
    switch (bt) {
        case BlockType::Function: return "function";
        case BlockType::Class: return "class";
        case BlockType::Method: return "method";
        case BlockType::Interface: return "interface";
        case BlockType::Struct: return "struct";
        case BlockType::Variable: return "variable";
        case BlockType::Block: return "block";
        case BlockType::Enum: return "enum";
        case BlockType::Trait: return "trait";
        case BlockType::Impl: return "impl";
        case BlockType::Module: return "module";
        case BlockType::Namespace: return "namespace";
        case BlockType::Constructor: return "constructor";
        case BlockType::Other: return "other";
    }
    return "unknown";
}

/// A block boundary in source code (function body, class body, etc.).
struct BlockBoundary {
    int start{};
    int end{};
    BlockType type{};
    std::string name;
    int depth{};
};

/// Type of scope in the scope hierarchy.
enum class ScopeType : uint8_t {
    Folder = 0,
    File,
    Package,
    Namespace,
    Class,
    Interface,
    Function,
    Method,
    Variable,
    Block,
    Struct,
};

/// Returns the string name for a ScopeType value.
constexpr std::string_view to_string(ScopeType st) {
    switch (st) {
        case ScopeType::Folder: return "folder";
        case ScopeType::File: return "file";
        case ScopeType::Package: return "package";
        case ScopeType::Namespace: return "namespace";
        case ScopeType::Class: return "class";
        case ScopeType::Interface: return "interface";
        case ScopeType::Function: return "function";
        case ScopeType::Method: return "method";
        case ScopeType::Variable: return "variable";
        case ScopeType::Block: return "block";
        case ScopeType::Struct: return "struct";
    }
    return "unknown";
}

/// Contextual scope information for a symbol or reference.
struct ScopeInfo {
    ScopeType type{};
    std::string name;
    std::string full_path;
    int start_line{};
    int end_line{};
    int level{};
    std::string language;
    std::vector<ContextAttribute> attributes;
};

/// Scope type for the symbol scope hierarchy (distinct from ScopeType).
enum class SymbolScopeType : uint8_t {
    Global = 0,
    Module,
    Package,
    Class,
    Function,
    Method,
    Block,
    Namespace,
    Interface,
};

/// Returns the string name for a SymbolScopeType value.
constexpr std::string_view to_string(SymbolScopeType sst) {
    switch (sst) {
        case SymbolScopeType::Global: return "global";
        case SymbolScopeType::Module: return "module";
        case SymbolScopeType::Package: return "package";
        case SymbolScopeType::Class: return "class";
        case SymbolScopeType::Function: return "function";
        case SymbolScopeType::Method: return "method";
        case SymbolScopeType::Block: return "block";
        case SymbolScopeType::Namespace: return "namespace";
        case SymbolScopeType::Interface: return "interface";
    }
    return "unknown";
}

/// Scope hierarchy node for a symbol.
struct SymbolScope {
    SymbolScopeType type{};
    std::string name;
    SymbolScope* parent{};
    int start_pos{};
    int end_pos{};
};

}  // namespace lci
