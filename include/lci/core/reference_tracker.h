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
#include <lci/core/atomic_shared_ptr.h>
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
    std::vector<ImportBinding> extract_csharp_imports(std::string_view match) const;
    std::vector<ImportBinding> extract_cpp_imports(std::string_view match) const;
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

    AtomicSharedPtr<const Snapshot> snapshot_;
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

    /// Returns all references where source or target is in the given file.
    std::vector<Reference> get_file_references(FileID file_id) const;

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
    /// Set to non-zero during bulk indexing to skip locking.
    std::atomic<int32_t> bulk_indexing{0};

    /// Opens (enabled=true) / closes (enabled=false) a bulk-index window.
    /// During the window writes accumulate into a private unpublished staging
    /// snapshot and a single atomic publish happens on close, avoiding the
    /// O(files^2) cost of cloning the growing snapshot per file. Outside the
    /// window writes are clone-mutate-publish (RCU) per call. Mirrors
    /// PostingsIndex / FileContentStore.
    void set_bulk_indexing(bool enabled);

    /// Applies parser-only metadata enrichment (complexity, signature, doc
    /// comment) to already-processed symbols. Replaces the old mutable
    /// symbol_store_mut() accessor: under the RCU model the SymbolStore lives
    /// inside an immutable snapshot, so enrichment is a single write that
    /// clone-mutate-publishes (or mutates the bulk staging snapshot). The
    /// integrator collects the enriched EnhancedSymbols and applies them in
    /// one call after process_file.
    void apply_enrichment(std::span<const EnhancedSymbol> enriched);

    // -- RCU snapshot --------------------------------------------------------

    /// Immutable read-side state, swapped atomically (RCU). Readers load the
    /// shared_ptr once (load_snapshot / pin) and operate on the frozen
    /// snapshot with no lock; writers clone-mutate-publish under write_mu_, or
    /// mutate the bulk staging snapshot in place and publish once on close.
    /// Mirrors FileContentStore / PostingsIndex. The read-side query logic
    /// that hands back pointers/views into this state lives here so callers
    /// that need pointer lifetime past the call (execute_search) can pin a
    /// snapshot and query it directly.
    struct Snapshot : std::enable_shared_from_this<Snapshot> {
        using SymbolHandle = std::shared_ptr<const EnhancedSymbol>;
        using LineMap = absl::flat_hash_map<int, std::vector<int>>;
        using LineMapHandle = std::shared_ptr<const LineMap>;

        Snapshot() = default;
        Snapshot(const Snapshot&) = default;
        Snapshot& operator=(const Snapshot&) = default;

        SymbolStore symbols{256};
        absl::flat_hash_map<uint64_t, Reference> references;
        absl::flat_hash_map<SymbolID, std::vector<uint64_t>> incoming_refs;
        absl::flat_hash_map<SymbolID, std::vector<uint64_t>> outgoing_refs;
        absl::flat_hash_map<FileID, std::vector<ScopeInfo>> scopes_by_file;
        absl::flat_hash_map<SymbolID, std::vector<ScopeInfo>> symbol_scopes;
        absl::flat_hash_map<FileID, absl::flat_hash_map<int, std::vector<int>>>
            line_to_symbols_by_file;
        ReferenceStats stats{};

        std::vector<SymbolHandle> find_symbols_by_name(
            std::string_view name) const;
        std::vector<Reference> get_symbol_references(
            SymbolID symbol_id, std::string_view direction) const;
        std::vector<Reference> get_references_by_id(
            std::span<const uint64_t> ref_ids) const;

        // Aliasing handles retain this snapshot while referring directly to
        // its immutable symbols/maps. Results therefore remain valid even if
        // the caller queried through a temporary pin.
        SymbolHandle get_enhanced_symbol(SymbolID symbol_id) const;
        std::vector<SymbolHandle> get_file_enhanced_symbols(
            FileID file_id) const;
        SymbolHandle find_symbol_by_name(std::string_view name) const;
        SymbolHandle find_symbol_by_file_and_name(
            FileID file_id, std::string_view name) const;
        LineMapHandle get_file_line_to_symbols(FileID file_id) const;
        // Line lookup deliberately uses only this snapshot. Combining it with
        // the separately-published SymbolLocationIndex can mix generations
        // during a concurrent reindex and resolve a reused ID to the wrong
        // symbol.
        SymbolHandle get_symbol_at_line(FileID file_id, int line) const;
    };

    /// Pins the current published snapshot. Symbol and line-map queries return
    /// aliasing handles that retain this snapshot, so their lifetime remains
    /// safe even after the explicit pin leaves scope.
    std::shared_ptr<const Snapshot> pin() const { return load_snapshot(); }

  private:
    AtomicSharedPtr<const Snapshot> snapshot_;
    mutable std::mutex write_mu_;
    /// Non-null only inside a bulk window (guarded by write_mu_).
    std::shared_ptr<Snapshot> staging_;

    std::shared_ptr<const Snapshot> load_snapshot() const;
    template <class Fn>
    void write_snapshot(Fn&& fn);

    ImportResolver import_resolver_;
    std::vector<FileImportData> import_data_;

    SymbolLocationIndex* symbol_location_index_{};

    absl::flat_hash_map<uint64_t, SymbolID> reference_cache_;
    absl::flat_hash_map<uint64_t, ScopeChainCacheEntry> scope_chain_cache_;

    /// Per-file resolution metadata derived from the path at process_file
    /// time. lang_group gates cross-language linking (a Python call must not
    /// resolve to a same-named C++ symbol in a vendored tree); low_quality
    /// demotes test/example/vendored files in the ambiguous-name fallback.
    struct FileResolutionMeta {
        uint8_t lang_group{0};  // 0 = unknown, otherwise a language family
        bool low_quality{false};
    };
    absl::flat_hash_map<FileID, FileResolutionMeta> file_resolution_meta_;

    SymbolID next_symbol_id_{1};
    uint64_t next_ref_id_{1};

    // -- Internal helpers ----------------------------------------------------

    // Write-path helpers mutate the snapshot being built (passed by ref).
    void remove_from_incoming_refs(Snapshot& s, SymbolID symbol_id,
                                   uint64_t ref_id);
    void remove_from_outgoing_refs(Snapshot& s, SymbolID symbol_id,
                                   uint64_t ref_id);

    static uint64_t make_global_ref_id(FileID file_id, uint32_t local_ref_id);
    static bool compute_is_exported(std::string_view path,
                                    std::string_view symbol_name);

    std::vector<ScopeInfo> build_symbol_scope_chain(
        const Symbol& symbol, std::span<const ScopeInfo> scopes);
    uint64_t create_scope_chain_cache_key(
        const Symbol& symbol, std::span<const ScopeInfo> scopes,
        int& scope_count_out) const;

    SymbolID find_symbol_at_location(const Snapshot& s, FileID file_id,
                                     int line, int col) const;
    SymbolID resolve_reference_target(
        const Snapshot& s, const Reference& ref,
        std::span<const SymbolID> file_symbol_ids);

    void update_reference_stats(Snapshot& s);
    void update_reference_stats_for_symbol(Snapshot& s, SymbolID symbol_id);

    std::vector<SymbolID> get_symbols_by_ref_type(
        SymbolID symbol_id, bool incoming, ReferenceType ref_type) const;

    FunctionTreeNode build_tree_node(
        const Snapshot& s, SymbolID symbol_id, int depth, int max_depth,
        absl::flat_hash_map<SymbolID, bool>& visited) const;

    static uint64_t fnv1a_hash_name(std::string_view name);
};

}  // namespace lci
