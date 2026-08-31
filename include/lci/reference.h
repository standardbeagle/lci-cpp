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
    RefStrength strength{};
    std::string referenced_name;
    bool ambiguous{};
    // Call goes through an explicit receiver that is not self/this. Such a
    // call can never be direct recursion, and when the receiver's type is
    // unknown the resolver must not guess a target from name evidence alone
    // (the false self-loop / reach-inflation class).
    bool foreign_receiver{};
    // The reference occurs in a TYPE position (declaration `Foo x;`, the
    // scope of `Foo::bar`, a base-class specifier): resolution considers
    // type-like symbols only. Without this, a class with a declared
    // constructor was name-ambiguous (class vs ctor) and every type use
    // built NO edge — C++ classes read as dead exports.
    bool type_position{};
    // No `quality`, `candidates`, or `failure_reason` here. They were carried
    // for every reference on every corpus and never written by any resolver,
    // never serialized, and never read -- 88 bytes of the struct plus their
    // allocator overhead, on the one object large corpora hold the most of.
    // Reinstate them only alongside the resolver that actually populates them.
};

/// An import statement in a source file.
struct Import {
    std::string path;
    FileID file_id{};
    int line{};
};

}  // namespace lci
