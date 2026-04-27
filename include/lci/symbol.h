#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <lci/types.h>

namespace lci {

/// Core symbol definition extracted from AST parsing.
/// Matches Go's types.Symbol struct.
struct Symbol {
    std::string name;
    SymbolType type{};
    FileID file_id{};
    int line{};
    int column{};
    int end_line{};
    int end_column{};
    std::vector<ContextAttribute> attributes;
    std::vector<TypeParameter> type_parameters;
    SymbolVisibility visibility{};
};

/// Forward declarations for types defined in other headers.
struct Reference;
struct ScopeInfo;
struct RefStats;

/// Extended symbol with relational information and compact metadata.
/// Matches Go's types.EnhancedSymbol struct.
struct EnhancedSymbol {
    Symbol symbol;
    SymbolID id{};
    std::vector<Reference> incoming_refs;
    std::vector<Reference> outgoing_refs;
    std::vector<ScopeInfo> scope_chain;

    // Enhanced metadata
    std::string type_info;
    bool is_mutable{};
    bool is_exported{};
    std::vector<std::string> annotations;
    std::string doc_comment;
    std::string signature;
    int complexity{};

    // Variable-specific metadata (compact bitfield representation)
    VariableType variable_type{};
    uint8_t variable_flags{};

    // Function-specific metadata (compact bitfield representation)
    uint8_t parameter_count{};
    uint8_t function_flags{};
    std::string receiver_type;

    // Variable flag accessors
    bool is_const() const { return (variable_flags & variable_flags::kConst) != 0; }
    bool is_static() const { return (variable_flags & variable_flags::kStatic) != 0; }
    bool is_pointer() const { return (variable_flags & variable_flags::kPointer) != 0; }
    bool is_array() const { return (variable_flags & variable_flags::kArray) != 0; }
    bool is_channel() const { return (variable_flags & variable_flags::kChannel) != 0; }
    bool is_interface() const { return (variable_flags & variable_flags::kInterface) != 0; }

    // Function flag accessors
    bool is_async_func() const { return (function_flags & function_flags::kAsync) != 0; }
    bool is_generator_func() const {
        return (function_flags & function_flags::kGenerator) != 0;
    }
    bool is_method_func() const { return (function_flags & function_flags::kMethod) != 0; }
    bool is_variadic_func() const {
        return (function_flags & function_flags::kVariadic) != 0;
    }
};

}  // namespace lci
