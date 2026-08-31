#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <lci/language_map.h>
#include <lci/path_classifier.h>
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

    /// Resolves which symbol a reference points to. `foreign_receiver`
    /// disables the unique-exported-candidate tier: for a call through an
    /// object of unknown type the real callee is often unindexed, and
    /// "the one exported name that happens to be in the corpus" is a guess,
    /// not evidence (the LOAD BEARING reach-inflation class).
    SymbolID resolve_symbol_reference(
        FileID ref_file_id,
        std::string_view referenced_name,
        std::span<const SymbolID> candidates,
        std::function<const EnhancedSymbol*(SymbolID)> symbol_lookup,
        bool foreign_receiver = false) const;

    void remove_file(FileID file_id);
    void clear();

  private:
    absl::flat_hash_map<FileID, std::vector<ImportBinding>> import_graph_;

    std::vector<ImportBinding> extract_go_imports(std::string_view match) const;
    std::vector<ImportBinding> extract_php_imports(std::string_view line) const;
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

/// Unique-postings-token cap policy, shared by the pipeline worker and the
/// incremental (watcher) path. Harm-based, not prediction-based: the cost a
/// file imposes on the postings index IS its unique-token count, and the
/// tokenizer counts it exactly -- so past a ceiling every file is capped,
/// whatever its bytes look like. Content classifiers only choose WHICH
/// ceiling:
///   - data files and detected payload sections: configured_cap
///   - code files: 4x configured_cap -- no legitimate source file carries
///     that vocabulary (biggest in this repo: ~8k uniques at the default
///     16k ceiling), so the wide ceiling is a pure backstop with zero
///     false-positive surface on normal code
/// 0 disables capping entirely.
inline size_t postings_token_cap(bool is_code_file, bool payload_content,
                                 int configured_cap) {
    if (configured_cap <= 0) return 0;
    const auto cap = static_cast<size_t>(configured_cap);
    return (is_code_file && !payload_content) ? cap * 4 : cap;
}

class PostingsIndex {
  public:
    /// Longest token retained by the tokenizer. Uniform on index AND
    /// implied for queries: a longer query misses postings everywhere,
    /// so the search engine's scan-all fallback answers it exactly.
    static constexpr size_t kMaxTokenBytes = 64;

    PostingsIndex();

    /// Indexes a file's content, recording first occurrence of each token.
    /// Tokenizes content inline; suitable for unit tests and single-file
    /// integration paths. max_unique_tokens caps the token set (0 = no
    /// cap); a capped file is recorded as PARTIAL and self-nominates in
    /// every find() so the prefilter stays a superset (see find()).
    void index_file(FileID file_id, std::string_view content,
                    size_t max_unique_tokens = 0);

    /// Indexes pre-tokenized postings produced by the parallel worker
    /// pool. Caller owns tokenization (see tokenize_content); this path
    /// just merges the (token, offset) pairs into tokens_/reverse_keys_
    /// without re-walking content. Single-threaded use only — meant to
    /// run on the FileIntegrator thread. truncated=true records the file
    /// as PARTIAL (its token set was capped during tokenization).
    void index_file_pretokenized(FileID file_id,
                                 std::vector<PostingsToken> tokens,
                                 bool truncated = false);

    /// Stateless tokenizer used by the pipeline worker pool. Extracts
    /// (token, first-offset) pairs from content using the same rules as
    /// index_file's internal scan (ASCII alnum/underscore tokens, ≥3
    /// chars after trim, lowercased, dedup by first occurrence).
    /// max_unique_tokens: stop collecting NEW tokens past this many
    /// (0 = unbounded); sets *truncated when the cap bit. Data files
    /// (word lists, JSON full of hex ids) have unbounded unique-token
    /// counts that the postings maps would retain at ~10x the file size;
    /// code files never hit a sane cap.
    static std::vector<PostingsToken> tokenize_content(
        std::string_view content, size_t max_unique_tokens = 0,
        bool* truncated = nullptr);

    /// Removes all postings for a file.
    void remove_file(FileID file_id);

    /// Finds candidate files for a token.
    /// Returns file IDs and their first-occurrence offsets. PARTIAL files
    /// (token set capped at index time) are always included, with offset
    /// -1: this index is a prefilter, and a prefilter is only correct as
    /// a SUPERSET of the true match set — a capped file must self-nominate
    /// for every query or tokens past its cap become silent misses. The
    /// content scan downstream discards the false positives.
    void find(std::string_view token, bool case_insensitive,
              std::vector<FileID>& files_out,
              absl::flat_hash_map<FileID, int>& offsets_out) const;

    /// Certified-absence narrowing for an arbitrary LITERAL pattern.
    ///
    /// `possible` is a strict SUPERSET of the files whose content can
    /// contain `pattern` as a substring, derived from the pattern's token
    /// runs ([A-Za-z0-9_]+, lowercased — postings store lowercase, so the
    /// superset holds for case-sensitive and -insensitive queries alike):
    ///   - a run delimited by non-token chars on both sides WITHIN the
    ///     pattern must appear as an exact stored token;
    ///   - a run touching the pattern's start may extend further left in
    ///     content, so any stored token with that SUFFIX qualifies;
    ///   - a run touching the pattern's end matches stored-token PREFIXes;
    ///   - a single-run pattern (both edges open) matches stored tokens
    ///     CONTAINING the run.
    /// Per-run file sets are intersected; PARTIAL files are then unioned
    /// in unconditionally (their token set is incomplete, so their absence
    /// from any lookup proves nothing).
    ///
    /// Runs shorter than 3 bytes or longer than kMaxTokenBytes are not
    /// indexed and contribute no constraint (dropping a conjunct keeps the
    /// superset). `informative` is false when NO run contributed — the
    /// caller must then scan without narrowing; `possible` is meaningless.
    struct Narrowing {
        bool informative{false};
        absl::flat_hash_set<FileID> possible;
    };
    Narrowing narrow(std::string_view pattern) const;

    /// Number of files whose token set was capped (recorded PARTIAL).
    int partial_file_count() const;

    /// Byte-census for `lci debug memprofile`: interned token string
    /// bytes, total (token,file) posting entries, and reverse-key
    /// entries. Estimates content, not container overhead.
    struct MemoryStats {
        size_t token_string_bytes{};
        size_t posting_entries{};
        size_t reverse_key_entries{};
    };
    MemoryStats memory_stats() const;

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
    /// Tokens are interned behind a dense uint32 id. The previous shape
    /// stored each token string once as the map key PLUS once per
    /// containing file in reverse_keys — "function" in 3000 files was
    /// 3001 string copies; reverse_keys was most of the index's bytes.
    /// Now the string lives once in token_to_id and every other
    /// appearance is 4 bytes. Ids are values, so RCU snapshot cloning
    /// stays a plain copy (a pointer/string_view scheme would dangle
    /// across clones). remove_file erases a token's file entries but
    /// keeps the id + string even when its postings empty: vocabulary is
    /// bounded and churn-stable, and reclaiming ids would need an
    /// id->string back-reference that reintroduces the second copy.
    struct Snapshot {
        absl::flat_hash_map<std::string, uint32_t> token_to_id;
        /// Indexed by token id; slot i belongs to the token whose
        /// token_to_id value is i. Never shrinks (see above). Each
        /// posting list is a plain (file, first-offset) vector -- 8 bytes
        /// per entry exactly. The previous per-token flat_hash_map cost
        /// ~200 B of fixed overhead per token plus ~2x slot slack (53k
        /// maps for 818k entries on the self-census); no read needs
        /// keyed lookup (find() copies the whole list out), and the
        /// double-index guard is per-file via reverse_keys, so the hash
        /// bought nothing.
        std::vector<std::vector<std::pair<FileID, int>>> postings;
        absl::flat_hash_map<FileID, std::vector<uint32_t>> reverse_keys;
        /// Files indexed with a capped token set. Unioned into every
        /// find() result — see find() for why the superset is mandatory.
        absl::flat_hash_set<FileID> partial_files;
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
                          std::string_view raw, int abs_start,
                          size_t max_unique_tokens, bool* truncated);
};

// ---------------------------------------------------------------------------
// ScopeChainCacheEntry - cached scope chain with collision verification
// ---------------------------------------------------------------------------

/// Hash-cons table entry, keyed by chain CONTENT. The first cut keyed on
/// the symbol's own line span, which made sharing structurally impossible
/// for siblings on different lines -- measured 19% dedup at next.js scale
/// where class members should share almost fully. Collisions are guarded
/// by full content comparison against the cached chain, not line fields.
struct ScopeChainCacheEntry {
    ScopeChain scope_chain;
};

// ---------------------------------------------------------------------------
// StoredRef - the reference AS STORED in the tracker snapshot
// ---------------------------------------------------------------------------

/// Storage twin of Reference. The public Reference is the materialized API
/// type; this is what the snapshot holds, one per reference on the object
/// large corpora hold the most of. Omitted relative to Reference:
///   - id:       derivable from the (file, slot) position in refs_by_file
///   - file_id:  the refs_by_file key
///   - referenced_name: a uint32 into the snapshot's append-only name
///     pool -- names repeat massively (every useState call re-stored
///     "useState"), 9.3 MB of per-ref strings at next.js scale
/// `dead` replaces the old id==0 tombstone convention explicitly.
/// 32 bytes vs Reference's 80 + string heap.
struct StoredRef {
    SymbolID source_symbol{};
    SymbolID target_symbol{};
    int line{};
    int column{};
    uint32_t name_id{};
    ReferenceType type{};
    RefStrength strength{};
    // Bitfields: three flags share one byte so the struct stays 32 bytes.
    bool ambiguous : 1 {};
    bool dead : 1 {};
    bool foreign_receiver : 1 {};
    bool type_position : 1 {};  // see Reference::type_position
};
static_assert(sizeof(StoredRef) == 32, "StoredRef is a per-reference cost");

// ---------------------------------------------------------------------------
// ReferenceTracker - bidirectional symbol references and scope relationships
// ---------------------------------------------------------------------------

class ReferenceTracker {
  public:
    /// The attribute set resolution reads (the Refs capability decides which
    /// files are preferred targets). Defaults to the shipped ruleset; the
    /// owning MasterIndex points it at the project's registry so `.lci.kdl`
    /// attributes reach resolution, which a private classifier here never
    /// allowed.
    void set_attr_registry(const PathAttrRegistry* registry) {
        attr_registry_ = registry != nullptr ? registry
                                             : &PathAttrRegistry::builtin();
    }
    const PathAttrRegistry& attr_registry() const { return *attr_registry_; }

    /// Language family for cross-language link gating. Now owned by the
    /// foundation extension table (lci::LangFamily in <lci/language_map.h>) so
    /// the map can key on it without a core dependency; aliased here to keep
    /// the ReferenceTracker::LangFamily::kXxx spelling used across the code.
    using LangFamily = lci::LangFamily;

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

    /// Resolved target IDs of ALL outgoing references (every type, duplicates
    /// preserved — coupling counts one edge per reference). Feeds the
    /// CouplingAnalyzer's targets_of callback.
    std::vector<SymbolID> get_outgoing_target_symbols(SymbolID symbol_id) const;
    std::vector<SymbolID> get_caller_symbols(SymbolID symbol_id) const;
    FunctionTreeNode build_function_tree(SymbolID symbol_id,
                                         int max_depth) const;

    // -- Statistics -----------------------------------------------------------

    ReferenceStats get_reference_stats() const;
    bool has_relationships() const;

    // -- Line-to-symbol index ------------------------------------------------

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
        Snapshot() = default;
        Snapshot(const Snapshot&) = default;
        Snapshot& operator=(const Snapshot&) = default;

        SymbolStore symbols{256};
        /// References grouped by owning file, local id = index + 1. The
        /// global ref id is (file_id << 32 | local_id), so it decomposes
        /// straight to a (file, index) slot -- no per-reference hash
        /// entry. The previous flat_hash_map<uint64, Reference> paid ~2x
        /// live bytes in slot overhead (90 MB slots for 42 MB live on the
        /// src/mono census). Per-symbol removal tombstones an entry by
        /// zeroing its id; a file reindex drops the whole vector, so
        /// tombstones only live between a symbol removal and its file's
        /// next reindex. Use find_ref()/for_each_live_ref(), which skip
        /// tombstones and bounds-check.
        absl::flat_hash_map<FileID, std::vector<StoredRef>> refs_by_file;
        /// Append-only interned name pool for StoredRef::name_id. deque:
        /// element addresses stable under append, copied whole on RCU
        /// clone (the single writer keeps the tracker-side intern table
        /// consistent with it). Never shrinks; vocabulary is bounded.
        std::deque<std::string> ref_names;
        absl::flat_hash_map<SymbolID, std::vector<uint64_t>> incoming_refs;
        absl::flat_hash_map<SymbolID, std::vector<uint64_t>> outgoing_refs;
        absl::flat_hash_map<FileID, std::vector<ScopeInfo>> scopes_by_file;
        ReferenceStats stats{};

        /// Resolves a global ref id to its stored slot; nullptr when the
        /// file is gone, the index is out of range, or the slot is dead.
        const StoredRef* find_ref(uint64_t ref_id) const;
        /// Materializes the public Reference shape from a stored slot.
        Reference materialize_ref(FileID file_id, uint32_t local_id,
                                  const StoredRef& r) const;
        /// Visits every live reference as (owning file, local id, stored).
        template <class Fn>
        void for_each_live_ref(Fn&& fn) const {
            for (const auto& [fid, vec] : refs_by_file) {
                for (uint32_t i = 0; i < vec.size(); ++i) {
                    if (!vec[i].dead) fn(fid, i + 1, vec[i]);
                }
            }
        }
        /// Count of live references (tombstones excluded).
        size_t live_ref_count() const;

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

    // Keyed on (file_id, FULL 64-bit name hash). The 64-bit half must stay
    // intact: a former key packed file_id into the high 32 bits and truncated
    // the name hash to the low 32, so two distinct names in one file collided
    // roughly once per 2^16 names and the loser silently resolved to the
    // winner's symbol.
    absl::flat_hash_map<std::pair<FileID, uint64_t>, SymbolID>
        reference_cache_;
    /// Writer-side intern table for Snapshot::ref_names (name -> pool id).
    absl::flat_hash_map<std::string, uint32_t> ref_name_ids_;
    absl::flat_hash_map<uint64_t, ScopeChainCacheEntry> scope_chain_cache_;  // hash-cons table
    const PathAttrRegistry* attr_registry_{&PathAttrRegistry::builtin()};

    /// Per-file resolution metadata derived from the path at process_file
    /// time. language_family gates cross-language linking; low_quality
    /// demotes test/example/vendored files in the ambiguous-name fallback.
    struct FileResolutionMeta {
        LangFamily language_family{LangFamily::kUnknown};
        bool low_quality{false};
        /// Hash of the parent directory — package-proximity tiebreak for
        /// ambiguous bare names (Go package = directory).
        uint64_t dir_hash{0};
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

    ScopeChain build_symbol_scope_chain(
        const Symbol& symbol, std::span<const ScopeInfo> scopes);


    SymbolID find_symbol_at_location(const Snapshot& s, FileID file_id,
                                     int line, int col) const;
    /// Interns a reference name into the snapshot pool (writer-side;
    /// the tracker-level table maps name -> pool id and is kept in sync
    /// by the single-writer discipline of write_snapshot).
    uint32_t intern_ref_name(Snapshot& s, std::string_view name);

    SymbolID resolve_reference_target(
        const Snapshot& s, const StoredRef& ref, FileID owner_fid,
        std::string_view name, std::span<const SymbolID> file_symbol_ids);

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
