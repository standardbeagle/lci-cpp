#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <lci/symbollinker/extractor.h>

struct TSNode;

namespace lci::symbollinker {

/// Extracts symbols from PHP source files using tree-sitter ASTs.
///
/// Handles namespace declarations, use statements, class/interface/trait/enum
/// types, functions, methods, properties, and constants. Follows PHP
/// visibility rules based on access modifiers.
class PhpExtractor final : public SymbolExtractor {
  public:
    PhpExtractor() = default;

    SymbolTable extract_symbols(FileID file_id, std::string_view content,
                                TSTree* tree) override;

    parser::Language language() const override {
        return parser::Language::PHP;
    }

    bool can_handle(std::string_view path) const override;

  private:
    // Extracts the namespace declaration from the file.
    static std::string extract_namespace(TSNode root,
                                         std::string_view content);

    // Extracts use statements (namespace imports).
    static void extract_use_statements(TSNode root, std::string_view content,
                                       SymbolTable& table);

    // Extracts a single use declaration.
    static void extract_use_declaration(TSNode node, std::string_view content,
                                        SymbolTable& table);

    // Extracts a use clause within a use declaration.
    static void extract_use_clause(TSNode node, std::string_view content,
                                   SymbolTable& table,
                                   std::string_view base_ns);

    // Extracts include/require statements.
    static void extract_includes(TSNode root, std::string_view content,
                                 SymbolTable& table);

    // Recursively extracts symbols from AST nodes.
    static void extract_from_node(TSNode node, std::string_view content,
                                  SymbolTable& table,
                                  std::string_view class_name);

    // Extracts a function definition.
    static void extract_function(TSNode node, std::string_view content,
                                 SymbolTable& table);

    // Extracts a class declaration.
    static void extract_class(TSNode node, std::string_view content,
                              SymbolTable& table);

    // Extracts an interface declaration.
    static void extract_interface(TSNode node, std::string_view content,
                                  SymbolTable& table);

    // Extracts a trait declaration.
    static void extract_trait(TSNode node, std::string_view content,
                              SymbolTable& table);

    // Extracts an enum declaration.
    static void extract_enum(TSNode node, std::string_view content,
                             SymbolTable& table);

    // Extracts a method declaration within a class/trait/interface.
    static void extract_method(TSNode node, std::string_view content,
                               SymbolTable& table,
                               std::string_view class_name);

    // Extracts a property declaration within a class/trait.
    static void extract_property(TSNode node, std::string_view content,
                                 SymbolTable& table,
                                 std::string_view class_name);

    // Extracts a constant declaration.
    static void extract_const(TSNode node, std::string_view content,
                              SymbolTable& table,
                              std::string_view class_name);

    // Reconstructs a qualified name from a qualified_name or namespace_name.
    static std::string extract_qualified_name(TSNode node,
                                              std::string_view content);

    // Checks if a node has "public" among its modifier children.
    static bool has_public_modifier(TSNode node, std::string_view content);

    // Adds a symbol to the table and returns its local ID.
    static uint32_t add_symbol(SymbolTable& table, std::string name,
                               bool exported);
};

/// Resolves PHP use statements and include paths to file IDs.
///
/// Supports PSR-4 namespace-to-directory resolution, builtin function/class
/// detection, and file include path resolution.
class PhpResolver final : public ImportResolver {
  public:
    explicit PhpResolver(std::string root_path);

    ModuleResolution resolve_import(std::string_view import_path,
                                    FileID from_file) override;

    parser::Language language() const override {
        return parser::Language::PHP;
    }

    /// Sets the file registry for path-to-FileID mapping.
    void set_file_registry(
        const absl::flat_hash_map<std::string, FileID>& registry);

    /// Adds a PSR-4 namespace mapping.
    void add_psr4_mapping(std::string_view ns_prefix,
                          std::string_view directory);

  private:
    // Checks if the import path looks like a file include.
    static bool is_file_path(std::string_view import_path);

    // Resolves a namespace import using PSR-4 rules.
    ModuleResolution resolve_namespace_import(
        std::string_view class_name) const;

    // Resolves a file include path.
    ModuleResolution resolve_file_include(std::string_view include_path,
                                          FileID from_file) const;

    // Checks if a namespace is a PHP builtin.
    static bool is_builtin(std::string_view ns);

    std::string root_path_;
    absl::flat_hash_map<std::string, FileID> file_registry_;
    absl::flat_hash_map<FileID, std::string> reverse_registry_;
    absl::flat_hash_map<std::string, std::string> psr4_mappings_;
};

}  // namespace lci::symbollinker
