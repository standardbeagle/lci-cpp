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

/// Extracts symbols from JavaScript and TypeScript source files.
///
/// Handles ES6 imports/exports, CommonJS require, classes, functions,
/// variable declarations, and TypeScript-specific constructs (interfaces,
/// type aliases, enums, type-only imports/exports).
class JSExtractor final : public SymbolExtractor {
  public:
    /// Creates a JS extractor. If `typescript` is true, handles .ts/.tsx
    /// extensions and TypeScript-specific constructs.
    explicit JSExtractor(bool typescript = false);

    SymbolTable extract_symbols(FileID file_id, std::string_view content,
                                TSTree* tree) override;

    parser::Language language() const override {
        return is_typescript_ ? parser::Language::TypeScript
                              : parser::Language::JavaScript;
    }

    bool can_handle(std::string_view path) const override;

  private:
    // Extracts ES6 import statements and CommonJS require calls.
    static void extract_imports(TSNode root, std::string_view content,
                                SymbolTable& table, bool is_ts);

    // Extracts a single ES6 import statement.
    static void extract_import_statement(TSNode node,
                                         std::string_view content,
                                         SymbolTable& table, bool is_ts);

    // Extracts export statements (named, default, re-exports).
    static void extract_exports(TSNode root, std::string_view content,
                                SymbolTable& table, bool is_ts);

    // Extracts a single export statement.
    static void extract_export_statement(TSNode node,
                                         std::string_view content,
                                         SymbolTable& table, bool is_ts);

    // Recursively extracts symbols from AST nodes.
    static void extract_from_node(TSNode node, std::string_view content,
                                  SymbolTable& table, bool is_ts);

    // Extracts function/class/variable/TS-specific declarations.
    static void extract_function(TSNode node, std::string_view content,
                                 SymbolTable& table);
    static void extract_class(TSNode node, std::string_view content,
                              SymbolTable& table);
    static void extract_variable_declaration(TSNode node,
                                             std::string_view content,
                                             SymbolTable& table);
    static void extract_interface(TSNode node, std::string_view content,
                                  SymbolTable& table);
    static void extract_type_alias(TSNode node, std::string_view content,
                                   SymbolTable& table);
    static void extract_enum(TSNode node, std::string_view content,
                             SymbolTable& table);

    // Checks if a node is exported (parent is export_statement).
    static bool is_node_exported(TSNode node, std::string_view content);

    // Adds a symbol to the table and returns its local ID.
    static uint32_t add_symbol(SymbolTable& table, std::string name,
                               bool exported);

    bool is_typescript_{};
};

/// Resolves JavaScript/TypeScript import paths to file IDs.
///
/// Supports relative imports, Node.js builtin detection, and
/// bare specifier resolution via file registry lookup.
class JSResolver final : public ImportResolver {
  public:
    explicit JSResolver(std::string root_path);

    ModuleResolution resolve_import(std::string_view import_path,
                                    FileID from_file) override;

    parser::Language language() const override {
        return parser::Language::JavaScript;
    }

    /// Sets the file registry for path-to-FileID mapping.
    void set_file_registry(
        const absl::flat_hash_map<std::string, FileID>& registry) override;

  private:
    // Resolves relative imports (./xxx, ../xxx).
    ModuleResolution resolve_relative(std::string_view import_path,
                                      std::string_view from_dir) const;

    // Tries to find a file with various JS/TS extensions.
    ModuleResolution try_resolve_file(std::string_view base_path) const;

    // Checks if an import is a Node.js builtin module.
    static bool is_builtin_module(std::string_view import_path);

    std::string root_path_;
    absl::flat_hash_map<std::string, FileID> file_registry_;
    absl::flat_hash_map<FileID, std::string> reverse_registry_;
};

/// Creates a TypeScript-specific resolver (same resolution logic,
/// but registered for the TypeScript language).
class TSResolver final : public ImportResolver {
  public:
    explicit TSResolver(std::string root_path);

    ModuleResolution resolve_import(std::string_view import_path,
                                    FileID from_file) override;

    parser::Language language() const override {
        return parser::Language::TypeScript;
    }

    /// Sets the file registry for path-to-FileID mapping.
    void set_file_registry(
        const absl::flat_hash_map<std::string, FileID>& registry) override;

  private:
    JSResolver inner_;
};

}  // namespace lci::symbollinker
