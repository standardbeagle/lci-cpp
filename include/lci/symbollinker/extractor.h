#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/parser/parser.h>
#include <lci/types.h>

struct TSTree;

namespace lci::symbollinker {

/// Resolved import/module resolution result.
struct ModuleResolution {
    std::string request_path;
    std::string resolved_path;
    FileID file_id{};
    bool is_external{};
    bool is_builtin{};
    std::string error;
};

/// Information about an import statement in a source file.
struct ImportInfo {
    uint32_t local_id{};
    std::string import_path;
    std::string alias;
    std::vector<std::string> imported_names;
    bool is_default{};
    bool is_namespace{};
    bool is_type_only{};
    int line{};
    int column{};
};

/// Information about an export statement in a source file.
struct ExportInfo {
    uint32_t local_id{};
    std::string exported_name;
    std::string local_name;
    bool is_default{};
    bool is_type_only{};
    bool is_re_export{};
    std::string source_path;
    int line{};
    int column{};
};

/// Per-file symbol extraction results used by the linker engine.
struct SymbolTable {
    FileID file_id{};
    parser::Language language{};
    std::vector<SymbolID> symbol_ids;
    std::vector<std::string> symbol_names;
    std::vector<ImportInfo> imports;
    std::vector<ExportInfo> exports;
    uint32_t next_local_id{1};
};

/// Abstract interface for language-specific symbol extraction.
///
/// Language-specific implementations (Go, JS, Python, etc.) derive from
/// this interface and register with the LinkerEngine. The engine dispatches
/// to the appropriate extractor based on file extension.
class SymbolExtractor {
  public:
    virtual ~SymbolExtractor() = default;

    /// Extracts symbols from the given parsed AST into a SymbolTable.
    virtual SymbolTable extract_symbols(FileID file_id,
                                        std::string_view content,
                                        TSTree* tree) = 0;

    /// Returns the language this extractor handles.
    virtual parser::Language language() const = 0;

    /// Returns true if this extractor can handle the given file path.
    virtual bool can_handle(std::string_view path) const = 0;
};

/// Abstract interface for resolving import paths to files.
///
/// Language-specific implementations resolve import paths according to
/// each language's module resolution rules (Go module paths, Node.js
/// resolution, Python package discovery, etc.).
class ImportResolver {
  public:
    virtual ~ImportResolver() = default;

    /// Resolves an import path from the perspective of a file.
    virtual ModuleResolution resolve_import(std::string_view import_path,
                                            FileID from_file) = 0;

    /// Returns the language this resolver handles.
    virtual parser::Language language() const = 0;

    /// Injects the engine's path -> FileID registry so resolve_import can map a
    /// resolved module path to a concrete FileID. LinkerEngine::link_symbols
    /// calls this on every resolver after all files are indexed. Default no-op
    /// for resolvers that don't need a registry (e.g. test stubs).
    virtual void set_file_registry(
        const absl::flat_hash_map<std::string, FileID>& /*registry*/) {}
};

}  // namespace lci::symbollinker
