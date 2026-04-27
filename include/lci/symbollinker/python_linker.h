#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <lci/symbollinker/extractor.h>

struct TSNode;

namespace lci::symbollinker {

/// Extracts symbols from Python source files using tree-sitter ASTs.
///
/// Handles module-level functions, classes, methods, variable assignments,
/// import statements (import, from...import), and async function definitions.
/// Follows Python visibility rules (underscore-prefixed names are private).
class PythonExtractor final : public SymbolExtractor {
  public:
    PythonExtractor() = default;

    SymbolTable extract_symbols(FileID file_id, std::string_view content,
                                TSTree* tree) override;

    parser::Language language() const override {
        return parser::Language::Python;
    }

    bool can_handle(std::string_view path) const override;

  private:
    // Extracts import statements (import and from...import).
    static void extract_imports(TSNode root, std::string_view content,
                                SymbolTable& table);

    // Extracts a simple import statement (import module).
    static void extract_import_statement(TSNode node,
                                         std::string_view content,
                                         SymbolTable& table);

    // Extracts a from...import statement.
    static void extract_import_from_statement(TSNode node,
                                              std::string_view content,
                                              SymbolTable& table);

    // Recursively extracts symbols from AST nodes.
    static void extract_from_node(TSNode node, std::string_view content,
                                  SymbolTable& table,
                                  std::string_view class_name);

    // Extracts a function or async function definition.
    static void extract_function(TSNode node, std::string_view content,
                                 SymbolTable& table,
                                 std::string_view class_name);

    // Extracts a class definition.
    static void extract_class(TSNode node, std::string_view content,
                              SymbolTable& table);

    // Extracts a variable assignment at module or class level.
    static void extract_assignment(TSNode node, std::string_view content,
                                   SymbolTable& table,
                                   std::string_view class_name);

    // Returns true if the name is considered exported (no leading underscore).
    static bool is_exported(std::string_view name);

    // Adds a symbol to the table and returns its local ID.
    static uint32_t add_symbol(SymbolTable& table, std::string name,
                               bool exported);
};

/// Resolves Python import paths to file IDs.
///
/// Supports builtin/stdlib detection, relative imports (.module, ..module),
/// absolute imports (package.module), and file registry lookup.
class PythonResolver final : public ImportResolver {
  public:
    explicit PythonResolver(std::string root_path);

    ModuleResolution resolve_import(std::string_view import_path,
                                    FileID from_file) override;

    parser::Language language() const override {
        return parser::Language::Python;
    }

    /// Sets the file registry for path-to-FileID mapping.
    void set_file_registry(
        const absl::flat_hash_map<std::string, FileID>& registry);

  private:
    // Resolves relative imports (.module, ..module).
    ModuleResolution resolve_relative(std::string_view import_path,
                                      std::string_view from_dir) const;

    // Resolves absolute imports (package.module) within the project.
    ModuleResolution resolve_absolute(std::string_view import_path) const;

    // Checks if an import is a Python builtin module.
    static bool is_builtin_module(std::string_view import_path);

    // Checks if an import is a Python standard library module.
    static bool is_stdlib_module(std::string_view import_path);

    std::string root_path_;
    absl::flat_hash_map<std::string, FileID> file_registry_;
    absl::flat_hash_map<FileID, std::string> reverse_registry_;
};

}  // namespace lci::symbollinker
