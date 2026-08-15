#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <lci/scope.h>
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
    // No attributes / type_parameters vectors here: nothing in the tree
    // ever wrote OR read them -- 48 bytes of empty vector per symbol on
    // every corpus. Reinstate only with the extractor that populates them.
    SymbolVisibility visibility{};
    // Declared parameter names for Function/Method symbols (0 otherwise).
    // Counted by the extractor at parse time; carried into
    // EnhancedSymbol::parameter_count, which the /list-symbols params
    // sort/filters read. Fits the struct's existing tail padding.
    uint8_t parameter_count{};
};

/// Forward declarations for types defined in other headers.
struct Reference;

/// Interned, immutable scope chain. Symbols with identical enclosing
/// scopes (every member of a class, every function in a module) share one
/// heap chain via hash-consing at build time; copying a symbol or cloning
/// a snapshot refcounts instead of deep-copying chain structs + strings.
/// The next.js-scale census measured 295k chain entries (42.5 MB structs
/// + 34.9 MB strings) as the largest structural term before interning.
/// Iteration API mirrors std::vector so read sites are unchanged.
/// shared_ptr is deliberate despite the no-shared_ptr-in-hot-paths rule:
/// chains are read on inspect/context paths (not /search), iteration
/// derefs once with zero refcount traffic, and the refcount is the entire
/// point at copy sites.
class ScopeChain {
  public:
    ScopeChain() = default;
    explicit ScopeChain(std::shared_ptr<const std::vector<ScopeInfo>> chain)
        : chain_(std::move(chain)) {}

    const ScopeInfo* begin() const { return storage().data(); }
    const ScopeInfo* end() const { return storage().data() + storage().size(); }
    size_t size() const { return storage().size(); }
    bool empty() const { return storage().empty(); }
    const ScopeInfo& operator[](size_t i) const { return storage()[i]; }
    const ScopeInfo& front() const { return storage().front(); }
    const ScopeInfo& back() const { return storage().back(); }
    /// Identity of the shared storage; census/dedup use only.
    const void* storage_key() const { return chain_.get(); }

  private:
    const std::vector<ScopeInfo>& storage() const {
        static const std::vector<ScopeInfo> kEmpty;
        return chain_ ? *chain_ : kEmpty;
    }
    std::shared_ptr<const std::vector<ScopeInfo>> chain_;
};

/// Extended symbol with relational information and compact metadata.
/// Matches Go's types.EnhancedSymbol struct.
struct EnhancedSymbol {
    Symbol symbol;
    SymbolID id{};
    // Reference COUNTS only. The references themselves live once in the
    // ReferenceTracker snapshot (references map + incoming/outgoing ID
    // lists); callers that need the actual Reference objects fetch them via
    // Snapshot::get_symbol_references(id, direction). The previous design
    // deep-copied every reference into both endpoint symbols — 2-3 copies
    // of a 168-byte struct with heap strings, the dominant index-memory
    // term on large corpora (2026-08-04 census).
    int incoming_ref_count{};
    int outgoing_ref_count{};
    ScopeChain scope_chain;

    // Enhanced metadata
    std::string type_info;
    bool is_mutable{};
    bool is_exported{};
    std::string doc_comment;
    std::string signature;
    int complexity{};

    // Variable-specific metadata (compact bitfield representation)
    VariableType variable_type{};
    uint8_t variable_flags{};

    // Function-specific metadata (compact bitfield representation)
    uint8_t parameter_count{};
    uint8_t function_flags{};
    // NOTE: receiver_type is currently written by NO extractor, yet it is
    // load-bearing API surface: the /list-symbols receiver filter
    // (server.cpp, handlers_explore.cpp) compares against it, so with no
    // writer that filter silently matches nothing -- a half-built feature,
    // tracked as a finding, not a deletable field.
    std::string receiver_type;
    // No annotations vector: never written by any extractor or enricher,
    // and every reader was guarded on !empty() (dead branches, deleted).

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
