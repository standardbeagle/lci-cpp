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

/// Extracts symbols from Go source files using tree-sitter ASTs.
///
/// Handles package-level functions, methods, struct/interface types,
/// var/const declarations, and import statements. Follows Go visibility
/// rules (capitalized identifiers are exported).
class GoExtractor final : public SymbolExtractor {
  public:
    GoExtractor() = default;

    SymbolTable extract_symbols(FileID file_id, std::string_view content,
                                TSTree* tree) override;

    parser::Language language() const override {
        return parser::Language::Go;
    }

    bool can_handle(std::string_view path) const override;

  private:
    // Extracts the package name from the root node.
    static std::string extract_package_name(TSNode root,
                                            std::string_view content);

    // Extracts all import declarations.
    static void extract_imports(TSNode root, std::string_view content,
                                SymbolTable& table);

    // Extracts a single import spec.
    static ImportInfo extract_import_spec(TSNode spec,
                                          std::string_view content);

    // Recursively extracts symbols from AST nodes.
    static void extract_from_node(TSNode node, std::string_view content,
                                  SymbolTable& table);

    // Extracts a function declaration.
    static void extract_function(TSNode node, std::string_view content,
                                 SymbolTable& table);

    // Extracts a method declaration (with receiver).
    static void extract_method(TSNode node, std::string_view content,
                               SymbolTable& table);

    // Extracts a type declaration (struct, interface, type alias).
    static void extract_type_declaration(TSNode node,
                                         std::string_view content,
                                         SymbolTable& table);

    // Extracts struct fields.
    static void extract_struct_fields(TSNode struct_node,
                                      std::string_view content,
                                      SymbolTable& table,
                                      std::string_view struct_name);

    // Extracts interface method signatures.
    static void extract_interface_methods(TSNode iface_node,
                                          std::string_view content,
                                          SymbolTable& table,
                                          std::string_view iface_name);

    // Extracts var/const declarations.
    static void extract_var_declaration(TSNode node,
                                        std::string_view content,
                                        SymbolTable& table, bool is_const);

    // Returns true if the name starts with an uppercase letter (exported).
    static bool is_exported(std::string_view name);

    // Adds a symbol to the table and returns its local ID.
    static uint32_t add_symbol(SymbolTable& table, std::string name,
                               bool exported);
};

/// Resolves Go import paths to file IDs using module path matching.
///
/// Supports standard library detection, module-relative imports,
/// and vendor directory lookups.
class GoResolver final : public ImportResolver {
  public:
    explicit GoResolver(std::string root_path);

    ModuleResolution resolve_import(std::string_view import_path,
                                    FileID from_file) override;

    parser::Language language() const override {
        return parser::Language::Go;
    }

    /// Sets the file registry for path-to-FileID mapping.
    void set_file_registry(
        const absl::flat_hash_map<std::string, FileID>& registry);

    /// Sets the module name (normally parsed from go.mod).
    void set_module_name(std::string_view name);

    /// Returns the module name.
    std::string_view module_name() const { return module_name_; }

  private:
    // Checks if an import path is a Go standard library package.
    static bool is_standard_package(std::string_view import_path);

    // Resolves imports that match the module prefix.
    ModuleResolution resolve_module_import(std::string_view import_path) const;

    std::string root_path_;
    std::string module_name_;
    absl::flat_hash_map<std::string, FileID> file_registry_;
};

}  // namespace lci::symbollinker
