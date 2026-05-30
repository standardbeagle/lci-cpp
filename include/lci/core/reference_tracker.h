#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/reference.h>
#include <lci/scope.h>
#include <lci/symbol.h>
#include <lci/types.h>
#include <lci/core/symbol_store.h>

namespace lci {

// ---------------------------------------------------------------------------
// ReferenceStats - pre-computed reference tracking statistics
// ---------------------------------------------------------------------------

struct ReferenceStats {
    int total_references{};
    int total_symbols{};
    int files_with_refs{};
    int symbol_refs{};
};

// ---------------------------------------------------------------------------
// TypeRelationships - all type relationship information for a symbol
// ---------------------------------------------------------------------------

struct TypeRelationships {
    std::vector<SymbolID> implements;
    std::vector<SymbolID> implemented_by;
    std::vector<SymbolID> extends;
    std::vector<SymbolID> extended_by;

    bool has_relationships() const;
};

// ---------------------------------------------------------------------------
// FunctionTreeNode - node in a function call tree
// ---------------------------------------------------------------------------

struct FunctionTreeNode {
    std::string name;
    std::vector<FunctionTreeNode> children;
    SymbolID symbol_id{};
    FileID file_id{};
    int line{};
};

// ---------------------------------------------------------------------------
// ImportBinding - a symbol import relationship
// ---------------------------------------------------------------------------

struct ImportBinding {
    std::string imported_name;
    std::string original_name;
    std::string source_file;
    int line_number{};
    bool is_wildcard{};
};

// ---------------------------------------------------------------------------
// FileImportData - import data for a single file
// ---------------------------------------------------------------------------

struct FileImportData {
    FileID file_id{};
    std::vector<ImportBinding> bindings;
};

// ---------------------------------------------------------------------------
// ImportResolver - language-agnostic heuristics for symbol resolution
// ---------------------------------------------------------------------------

class ImportResolver {
  public:
    ImportResolver() = default;

    /// Extracts import data from a file (lock-free, safe for parallel calls).
    FileImportData extract_file_imports(FileID file_id,
                                        std::string_view file_path,
                                        std::string_view content);

    /// Builds the import graph from collected data (single-threaded).
    void build_import_graph(std::span<const FileImportData> import_data);

    /// Resolves which symbol a reference points to.
    SymbolID resolve_symbol_reference(
        FileID ref_file_id,
        std::string_view referenced_name,
        std::span<const SymbolID> candidates,
        std::function<const EnhancedSymbol*(SymbolID)> symbol_lookup) const;

    void remove_file(FileID file_id);
    void clear();

  private:
    absl::flat_hash_map<FileID, std::vector<ImportBinding>> import_graph_;

    std::vector<ImportBinding> extract_go_imports(std::string_view match) const;
    std::vector<ImportBinding> extract_js_imports(std::string_view match) const;
    std::vector<ImportBinding> extract_python_imports(std::string_view match) const;
    std::vector<ImportBinding> extract_rust_imports(std::string_view match) const;
};

// ---------------------------------------------------------------------------
// PostingsIndex - token -> (fileID -> firstOffset) index
// ---------------------------------------------------------------------------

/// Token + first-occurrence-offset extracted from a single file.
/// Mirrors the inline definition in pipeline_types.h for use outside the
/// indexing pipeline (e.g. tests).
struct PostingsToken {
    std::string token;
    int offset{};
};

class PostingsIndex {
  public:
    PostingsIndex();

    /// Indexes a file's content, recording first occurrence of each token.
    /// Tokenizes content inline; suitable for unit tests and single-file
    /// integration paths.
    void index_file(FileID file_id, std::string_view content);

    /// Indexes pre-tokenized postings produced by the parallel worker
    /// pool. Caller owns tokenization (see tokenize_content); this path
    /// just merges the (token, offset) pairs into tokens_/reverse_keys_
    /// without re-walking content. Single-threaded use only — meant to
    /// run on the FileIntegrator thread.
    void index_file_pretokenized(FileID file_id,
                                 std::vector<PostingsToken> tokens);

    /// Stateless tokenizer used by the pipeline worker pool. Extracts
    /// (token, first-offset) pairs from content using the same rules as
    /// index_file's internal scan (ASCII alnum/underscore tokens, ≥3
    /// chars after trim, lowercased, dedup by first occurrence).
    static std::vector<PostingsToken> tokenize_content(
        std::string_view content);

    /// Removes all postings for a file.
    void remove_file(FileID file_id);

    /// Finds candidate files for a token.
    /// Returns file IDs and their first-occurrence offsets.
    void find(std::string_view token, bool case_insensitive,
              std::vector<FileID>& files_out,
              absl::flat_hash_map<FileID, int>& offsets_out) const;

    /// Returns the number of distinct tokens indexed.
    int token_count() const;

    /// Returns the number of files indexed.
    int file_count() const;

    void clear();

    /// Opens (enabled=true) or closes (enabled=false) a bulk-build window.
    /// During the window, writes accumulate into a private unpublished
    /// snapshot and a single atomic publish happens on close — avoiding the
    /// O(files^2) cost of cloning the growing tokens map per file. Outside
    /// the window writes are clone-mutate-publish (RCU) per call.
    void set_bulk_indexing(bool enabled);

  private:
    /// Immutable read-side state, swapped atomically (RCU). Readers load
    /// the shared_ptr once and operate on the frozen snapshot with zero
    /// locks; writers clone-mutate-publish under write_mu_ (or, during a
    /// bulk window, mutate staging_ in place and publish once). Mirrors
    /// FileContentStore's snapshot model.
    struct Snapshot {
        absl::flat_hash_map<std::string, absl::flat_hash_map<FileID, int>>
            tokens;
        absl::flat_hash_map<FileID, std::vector<std::string>> reverse_keys;
    };

    std::atomic<std::shared_ptr<const Snapshot>> snapshot_;
    mutable std::mutex write_mu_;
    /// Non-null only inside a bulk window (guarded by write_mu_).
    std::shared_ptr<Snapshot> staging_;

    /// Loads the current published read snapshot (lock-free).
    std::shared_ptr<const Snapshot> load_snapshot() const;

    /// Applies a mutation to the index: in the bulk staging snapshot when a
    /// bulk window is open, otherwise clone-mutate-publish.
    template <class Fn>
    void write_snapshot(Fn&& fn);

    static bool is_token_char(uint8_t b);
    static bool is_all_ascii(std::string_view s);
    static void add_token(absl::flat_hash_map<std::string, int>& dst,
                          std::string_view raw, int abs_start);
};

// ---------------------------------------------------------------------------
// ScopeChainCacheEntry - cached scope chain with collision verification
// ---------------------------------------------------------------------------

struct ScopeChainCacheEntry {
    std::vector<ScopeInfo> scope_chain;
    int symbol_line{};
    int symbol_end_line{};
    int scope_count{};
};

// ---------------------------------------------------------------------------
// ReferenceTracker - bidirectional symbol references and scope relationships
// ---------------------------------------------------------------------------

class ReferenceTracker {
  public:
    explicit ReferenceTracker(SymbolLocationIndex* location_index = nullptr);

    // -- File processing -----------------------------------------------------

    /// Processes a file's symbols, references, and scopes.
    /// Returns the enhanced symbols created.
    std::vector<EnhancedSymbol> process_file(
        FileID file_id, std::string_view path,
        std::span<const Symbol> symbols,
        std::span<const Reference> references,
        std::span<const ScopeInfo> scopes);

    /// Processes a file's imports for symbol resolution.
    void process_file_imports(FileID file_id, std::string_view file_path,
                              std::string_view content);

    /// Processes all stored references after all symbols have been indexed.
    void process_all_references();

    /// Removes all symbols and references for a file.
    void remove_file(FileID file_id);

    /// Resets all data.
    void clear();

    // -- Query methods -------------------------------------------------------

    /// Returns references for a symbol in a given direction.
    /// direction: "incoming", "outgoing", or "both".
    std::vector<Reference> get_symbol_references(
        SymbolID symbol_id, std::string_view direction) const;

    /// Finds all symbols with a given name.
    std::vector<const EnhancedSymbol*> find_symbols_by_name(
        std::string_view name) const;

    /// Returns an enhanced symbol by ID. Returns nullptr if not found.
    const EnhancedSymbol* get_enhanced_symbol(SymbolID symbol_id) const;

    /// Returns all enhanced symbols for a file.
    std::vector<const EnhancedSymbol*> get_file_enhanced_symbols(
        FileID file_id) const;

    /// Returns all references where source or target is in the given file.
    std::vector<Reference> get_file_references(FileID file_id) const;

    /// Finds the symbol containing a given line in a file.
    const EnhancedSymbol* get_symbol_at_line(FileID file_id, int line) const;

    /// Finds a symbol by name (first match).
    const EnhancedSymbol* find_symbol_by_name(std::string_view name) const;

    /// Finds a symbol by file ID and name.
    const EnhancedSymbol* find_symbol_by_file_and_name(
        FileID file_id, std::string_view name) const;

    /// Returns a snapshot of all references.
    std::vector<Reference> get_all_references() const;

    // -- Type relationship queries -------------------------------------------

    std::vector<SymbolID> get_implementors(SymbolID interface_id) const;
    std::vector<SymbolID> get_implemented_interfaces(SymbolID type_id) const;
    std::vector<SymbolID> get_base_types(SymbolID type_id) const;
    std::vector<SymbolID> get_derived_types(SymbolID base_id) const;
    TypeRelationships get_type_relationships(SymbolID symbol_id) const;

    // -- Call graph utilities ------------------------------------------------

    std::vector<std::string> get_callee_names(SymbolID symbol_id) const;
    std::vector<std::string> get_caller_names(SymbolID symbol_id) const;
    std::vector<SymbolID> get_callee_symbols(SymbolID symbol_id) const;
    std::vector<SymbolID> get_caller_symbols(SymbolID symbol_id) const;
    FunctionTreeNode build_function_tree(SymbolID symbol_id,
                                         int max_depth) const;

    // -- Statistics -----------------------------------------------------------

    ReferenceStats get_reference_stats() const;
    bool has_relationships() const;

    // -- Line-to-symbol index ------------------------------------------------

    void store_line_to_symbols(
        FileID file_id,
        absl::flat_hash_map<int, std::vector<int>> line_to_symbols);
    const absl::flat_hash_map<int, std::vector<int>>*
        get_file_line_to_symbols(FileID file_id) const;

    /// Set to non-zero during bulk indexing to skip locking.
    std::atomic<int32_t> bulk_indexing{0};

    /// Returns a mutable reference to the underlying SymbolStore so
    /// post-processing stages (e.g. integrator metadata enrichment) can
    /// update fields not surfaced by `process_file`. Const callers should
    /// continue to go through the typed accessors above.
    SymbolStore& symbol_store_mut() { return symbols_; }

  private:
    SymbolStore symbols_;
    absl::flat_hash_map<uint64_t, Reference> references_;
    absl::flat_hash_map<SymbolID, std::vector<uint64_t>> incoming_refs_;
    absl::flat_hash_map<SymbolID, std::vector<uint64_t>> outgoing_refs_;

    absl::flat_hash_map<FileID, std::vector<ScopeInfo>> scopes_by_file_;
    absl::flat_hash_map<SymbolID, std::vector<ScopeInfo>> symbol_scopes_;

    ImportResolver import_resolver_;
    std::vector<FileImportData> import_data_;

    SymbolLocationIndex* symbol_location_index_{};

    absl::flat_hash_map<uint64_t, SymbolID> reference_cache_;
    absl::flat_hash_map<uint64_t, ScopeChainCacheEntry> scope_chain_cache_;

    absl::flat_hash_map<FileID, absl::flat_hash_map<int, std::vector<int>>>
        line_to_symbols_by_file_;

    SymbolID next_symbol_id_{1};
    uint64_t next_ref_id_{1};

    ReferenceStats stats_{};

    // -- Internal helpers ----------------------------------------------------

    void remove_from_incoming_refs(SymbolID symbol_id, uint64_t ref_id);
    void remove_from_outgoing_refs(SymbolID symbol_id, uint64_t ref_id);

    static uint64_t make_global_ref_id(FileID file_id, uint32_t local_ref_id);
    static bool compute_is_exported(std::string_view path,
                                    std::string_view symbol_name);

    std::vector<ScopeInfo> build_symbol_scope_chain(
        const Symbol& symbol, std::span<const ScopeInfo> scopes);
    uint64_t create_scope_chain_cache_key(
        const Symbol& symbol, std::span<const ScopeInfo> scopes,
        int& scope_count_out) const;

    SymbolID find_symbol_at_location(FileID file_id, int line, int col) const;
    SymbolID resolve_reference_target(
        const Reference& ref,
        std::span<const SymbolID> file_symbol_ids);

    std::vector<Reference> get_references_by_id(
        std::span<const uint64_t> ref_ids) const;

    void update_reference_stats();
    void update_reference_stats_for_symbol(SymbolID symbol_id);

    std::vector<SymbolID> get_symbols_by_ref_type(
        SymbolID symbol_id, bool incoming, ReferenceType ref_type) const;

    FunctionTreeNode build_tree_node(
        SymbolID symbol_id, int depth, int max_depth,
        absl::flat_hash_map<SymbolID, bool>& visited) const;

    static uint64_t fnv1a_hash_name(std::string_view name);
};

}  // namespace lci
