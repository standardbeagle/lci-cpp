#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <lci/symbollinker/extractor.h>

// tree-sitter declares TSNode inside `extern "C"`; the forward declaration
// must match that language linkage (gcc-13/clang-18 reject a mismatch).
extern "C" {
struct TSNode;
}

namespace lci::symbollinker {

/// Extracts symbols from C# source files using tree-sitter ASTs.
///
/// Handles namespace declarations, using directives, class/struct/interface/
/// enum/record types, methods, constructors, properties, fields, and
/// delegates. Follows C# visibility rules based on access modifiers.
class CSharpExtractor final : public SymbolExtractor {
  public:
    CSharpExtractor() = default;

    SymbolTable extract_symbols(FileID file_id, std::string_view content,
                                TSTree* tree) override;

    parser::Language language() const override {
        return parser::Language::CSharp;
    }

    bool can_handle(std::string_view path) const override;

  private:
    // Extracts using directives.
    static void extract_usings(TSNode root, std::string_view content,
                               SymbolTable& table);

    // Extracts a single using directive.
    static ImportInfo extract_using_directive(TSNode node,
                                             std::string_view content);

    // Extracts the namespace name from the root node.
    static std::string extract_namespace(TSNode root,
                                         std::string_view content);

    // Recursively extracts symbols from AST nodes.
    static void extract_from_node(TSNode node, std::string_view content,
                                  SymbolTable& table,
                                  std::string_view class_name);

    // Extracts a class, struct, interface, record, or enum declaration.
    static void extract_type_declaration(TSNode node,
                                         std::string_view content,
                                         SymbolTable& table);

    // Extracts a method declaration.
    static void extract_method(TSNode node, std::string_view content,
                               SymbolTable& table,
                               std::string_view class_name);

    // Extracts a constructor declaration.
    static void extract_constructor(TSNode node, std::string_view content,
                                    SymbolTable& table,
                                    std::string_view class_name);

    // Extracts a property declaration.
    static void extract_property(TSNode node, std::string_view content,
                                 SymbolTable& table,
                                 std::string_view class_name);

    // Extracts a field declaration.
    static void extract_field(TSNode node, std::string_view content,
                              SymbolTable& table,
                              std::string_view class_name);

    // Extracts enum members from an enum declaration body.
    static void extract_enum_members(TSNode body, std::string_view content,
                                     SymbolTable& table,
                                     std::string_view enum_name);

    // Checks if a node has "public" among its modifier children.
    static bool has_public_modifier(TSNode node, std::string_view content);

    // Adds a symbol to the table and returns its local ID.
    static uint32_t add_symbol(SymbolTable& table, std::string name,
                               bool exported);
};

/// Resolves C# using directives (namespace references) to file IDs.
///
/// Supports .NET builtin namespace detection, project namespace matching
/// via directory structure, and known third-party namespace patterns.
class CSharpResolver final : public ImportResolver {
  public:
    explicit CSharpResolver(std::string root_path);

    ModuleResolution resolve_import(std::string_view import_path,
                                    FileID from_file) override;

    parser::Language language() const override {
        return parser::Language::CSharp;
    }

    /// Sets the file registry for path-to-FileID mapping.
    void set_file_registry(
        const absl::flat_hash_map<std::string, FileID>& registry) override;

  private:
    // Resolves a namespace by searching project directory structure.
    ModuleResolution resolve_project_namespace(
        std::string_view ns) const;

    // Checks if a namespace is a .NET builtin.
    static bool is_builtin_namespace(std::string_view ns);

    // Checks if a namespace matches known third-party patterns.
    static bool is_known_external(std::string_view ns);

    std::string root_path_;
    absl::flat_hash_map<std::string, FileID> file_registry_;
    absl::flat_hash_map<FileID, std::string> reverse_registry_;
};

}  // namespace lci::symbollinker
