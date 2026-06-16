#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/parser/parser.h>
#include <lci/symbollinker/extractor.h>
#include <lci/types.h>

namespace lci::symbollinker {

/// Cross-file symbol relationship.
struct SymbolLink {
    SymbolID symbol{};
    FileID definition_file{};
    std::vector<FileID> imported_by;
    FileID exported_by{};
    bool is_external{};
};

/// Resolved import relationship between files.
struct ImportLink {
    FileID from_file{};
    std::string import_path;
    FileID resolved_file{};
    std::vector<std::string> imported_symbols;
    ModuleResolution resolution;
    bool is_external{};
};

/// Type of incremental file update.
enum class UpdateType : uint8_t {
    Modified = 0,
    Added,
    Removed,
    Cascade,
};

/// Returns the string name for an UpdateType value.
constexpr std::string_view to_string(UpdateType ut) {
    switch (ut) {
        case UpdateType::Modified: return "modified";
        case UpdateType::Added: return "added";
        case UpdateType::Removed: return "removed";
        case UpdateType::Cascade: return "cascade";
    }
    return "unknown";
}

/// Pending file update information.
struct UpdateInfo {
    FileID file_id{};
    std::string content;
    uint64_t content_hash{};
    UpdateType update_type{};
};

/// Result of an incremental update operation.
struct UpdateResult {
    std::vector<FileID> updated_files;
    std::vector<FileID> affected_files;
    int removed_symbols{};
    int added_symbols{};
    int modified_links{};
    std::chrono::microseconds duration{};
    int cascade_depth{};
};

/// Engine statistics snapshot.
struct LinkerStats {
    int files{};
    int symbol_links{};
    int import_links{};
    int extractors{};
    int resolvers{};
    int dependency_edges{};
};

/// Main engine for cross-file symbol linking with incremental support.
///
/// Dispatches to registered language-specific extractors and resolvers.
/// Supports incremental updates when individual files change, propagating
/// link changes through the dependency graph.
///
/// Thread safety: Caller is responsible for synchronization. Designed to be
/// used within a component that provides its own locking.
class LinkerEngine {
  public:
    explicit LinkerEngine(std::string root_path);

    // -- Extractor/resolver registration ------------------------------------

    /// Registers a symbol extractor for a language.
    void register_extractor(std::unique_ptr<SymbolExtractor> extractor);

    /// Registers an import resolver for a language.
    void register_resolver(std::unique_ptr<ImportResolver> resolver);

    // -- File management ----------------------------------------------------

    /// Gets or creates a FileID for a path.
    FileID get_or_create_file_id(std::string_view path);

    /// Gets the file path for a FileID. Returns empty string if not found.
    std::string_view get_file_path(FileID file_id) const;

    // -- Indexing and linking -----------------------------------------------

    /// Extracts symbols from a file and stores them in the engine.
    /// Returns false if no extractor can handle the file.
    bool index_file(std::string_view path, std::string_view content);

    /// Returns true if a registered extractor can handle this path (by
    /// extension). Lets callers skip reading files no linker would index
    /// without duplicating the per-language extension lists.
    bool can_index(std::string_view path) const;

    /// Performs cross-file symbol linking for all indexed files.
    /// Returns false if any file fails to link.
    bool link_symbols();

    // -- Incremental updates ------------------------------------------------

    /// Incrementally updates a single file. Returns update statistics.
    UpdateResult update_file(std::string_view path, std::string_view content);

    /// Incrementally removes a file. Returns update statistics.
    UpdateResult remove_file(std::string_view path);

    // -- Queries ------------------------------------------------------------

    /// Returns import links for a file.
    std::vector<const ImportLink*> get_file_imports(FileID file_id) const;

    /// Returns files that depend on the given file (import from it).
    std::vector<FileID> get_file_dependents(FileID file_id) const;

    /// Returns files that the given file depends on.
    std::vector<FileID> get_file_dependencies(FileID file_id) const;

    /// Returns the content hash for a file. Returns 0 if not tracked.
    uint64_t get_file_hash(FileID file_id) const;

    /// Returns the symbol table for a file. Returns nullptr if not indexed.
    const SymbolTable* get_symbol_table(FileID file_id) const;

    /// Returns engine statistics.
    LinkerStats stats() const;

    /// Returns the root path.
    std::string_view root_path() const { return root_path_; }

  private:
    // Finds an extractor that can handle the given file path.
    SymbolExtractor* find_extractor(std::string_view path) const;

    // Finds the resolver for a language.
    ImportResolver* find_resolver(parser::Language lang) const;

    // Processes all symbol links for a single file.
    bool process_file_links(FileID file_id, const SymbolTable& table);

    // Processes a single import statement.
    void process_import(FileID file_id, const ImportInfo& import_info,
                        parser::Language language);

    // Updates the dependency graph for a file based on its import links.
    void update_dependency_graph(FileID file_id);

    // Processes pending incremental updates with cascade propagation.
    UpdateResult process_pending_updates();

    // Processes a single file add/modify update.
    bool process_file_add_or_modify(const UpdateInfo& info,
                                    UpdateResult& result);

    // Processes a single file removal.
    void process_file_removal(const UpdateInfo& info, UpdateResult& result);

    // Processes a cascade re-link (no re-index).
    void process_file_cascade(const UpdateInfo& info, UpdateResult& result);

    // Finds files needing cascade updates due to a change.
    void find_cascade_updates(
        FileID changed_file,
        absl::flat_hash_map<FileID, UpdateInfo>& cascade_updates) const;

    // Removes a FileID from a vector.
    static void remove_file_from_vec(std::vector<FileID>& vec, FileID id);

    // Checks if a vector contains a FileID.
    static bool contains_file_id(const std::vector<FileID>& vec, FileID id);

    std::string root_path_;

    // Extractors and resolvers indexed by Language enum.
    std::array<std::unique_ptr<SymbolExtractor>, parser::kLanguageCount>
        extractors_{};
    std::array<std::unique_ptr<ImportResolver>, parser::kLanguageCount>
        resolvers_{};

    // Symbol storage
    absl::flat_hash_map<FileID, SymbolTable> symbol_tables_;
    absl::flat_hash_map<std::string, FileID> file_registry_;
    absl::flat_hash_map<FileID, std::string> reverse_registry_;
    FileID next_file_id_{1};

    // Cross-file links
    absl::flat_hash_map<SymbolID, SymbolLink> symbol_links_;
    absl::flat_hash_map<FileID, std::vector<ImportLink>> import_links_;

    // Incremental tracking
    absl::flat_hash_map<FileID, uint64_t> file_hashes_;
    absl::flat_hash_map<FileID, std::vector<FileID>> file_dependents_;
    absl::flat_hash_map<FileID, std::vector<FileID>> import_graph_;
    absl::flat_hash_map<FileID, UpdateInfo> pending_updates_;
};

/// Registers every built-in language linker pair (extractor + resolver) on the
/// engine: Go, Python, JavaScript, TypeScript, C#, and PHP. These are the
/// languages that have a SymbolLinker import-dependency pair today. Languages
/// that LCI parses but has no linker pair for — C/C++, Rust, Java, Kotlin, Zig,
/// Ruby — are intentionally absent; their files are skipped by the
/// dependency-graph path (production cross-file *symbol* resolution for all
/// parsed languages goes through ReferenceTracker, not this engine).
void register_all_linkers(LinkerEngine& engine, const std::string& root);

}  // namespace lci::symbollinker
