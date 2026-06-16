#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/core/atomic_shared_ptr.h>
#include <lci/reference.h>
#include <lci/symbol.h>
#include <lci/types.h>

namespace lci {

/// Pre-computed statistics about the symbol store contents.
struct SymbolStoreStats {
    int total_symbols{};
    int total_functions{};
    int total_methods{};
    int total_classes{};
    int total_variables{};
    int total_constants{};
    int files_indexed{};
};

/// High-performance symbol storage using parallel arrays for cache locality.
///
/// Instead of map<SymbolID, EnhancedSymbol*>, uses:
///   - vector<EnhancedSymbol> (data) + flat_hash_map<SymbolID, int> (index)
///
/// This provides O(1) array access (faster than map lookup), better CPU cache
/// locality, and reduced memory overhead.
///
/// Thread safety: Caller is responsible for synchronization. Designed to be
/// used within a component that provides its own locking.
class SymbolStore {
  public:
    explicit SymbolStore(int expected_size = 0);

    /// Retrieves a symbol by ID in O(1) time. Returns nullptr if not found.
    const EnhancedSymbol* get(SymbolID id) const;

    /// Retrieves a mutable symbol by ID. Returns nullptr if not found.
    EnhancedSymbol* get_mutable(SymbolID id);

    /// Adds or updates a symbol. If ID exists, updates in place.
    void set(SymbolID id, EnhancedSymbol symbol);

    /// Removes a symbol by ID using swap-and-delete. Returns true if removed.
    bool remove(SymbolID id);

    /// Returns the number of symbols in the store.
    int size() const;

    /// Returns all symbols as a read-only span.
    std::span<const EnhancedSymbol> get_all() const;

    /// Calls fn for each symbol. Stops if fn returns false.
    template <typename Fn>
    void range(Fn&& fn) const {
        for (int i = 0; i < static_cast<int>(data_.size()); ++i) {
            if (!fn(reverse_index_[static_cast<size_t>(i)],
                    data_[static_cast<size_t>(i)])) {
                return;
            }
        }
    }

    /// Returns all symbol IDs.
    std::vector<SymbolID> get_ids() const;

    /// Removes all symbols.
    void clear();

    /// Returns the current capacity of the data array.
    int capacity() const;

    // -- Secondary indices ----------------------------------------------------

    /// Returns symbols in a specific file.
    std::span<const SymbolID> get_symbols_by_file(FileID file_id) const;

    /// Returns symbols matching a name.
    std::span<const SymbolID> get_symbols_by_name(std::string_view name) const;

    /// Returns entry-point symbols (exported functions/methods).
    std::vector<SymbolID> get_entry_points() const;

    /// Returns top symbols sorted by reference count (most referenced first).
    std::vector<SymbolID> get_top_symbols(int limit) const;

    /// Returns current statistics.
    const SymbolStoreStats& stats() const;

    /// Rebuilds secondary indices and statistics from current data.
    void rebuild_indices();

  private:
    std::vector<EnhancedSymbol> data_;
    absl::flat_hash_map<SymbolID, int> index_;
    std::vector<SymbolID> reverse_index_;

    absl::flat_hash_map<FileID, std::vector<SymbolID>> symbols_by_file_;
    absl::flat_hash_map<std::string, std::vector<SymbolID>> symbols_by_name_;

    SymbolStoreStats stats_{};

    void update_stats_add(const EnhancedSymbol& sym);
    void update_stats_remove(const EnhancedSymbol& sym);
    void add_to_secondary_indices(SymbolID id, const EnhancedSymbol& sym);
    void remove_from_secondary_indices(SymbolID id, const EnhancedSymbol& sym);
};

// ---------------------------------------------------------------------------
// Position-based symbol lookup
// ---------------------------------------------------------------------------

/// A line/column coordinate in a file.
struct Position {
    int line{};
    int column{};
};

/// A symbol with its exact position range in a file.
struct PositionedSymbol {
    Symbol symbol;
    SymbolID symbol_id{};
    Position start;
    Position end;
};

/// Per-file spatial index storing symbols sorted by start line for binary search.
struct FileSymbolMap {
    /// All symbols sorted by start line (ascending), then by span size (ascending).
    std::vector<PositionedSymbol> sorted_symbols;
};

/// Spatial index for instant symbol lookup by file position.
///
/// Uses sorted intervals per file with binary search. Position queries use
/// upper_bound to find candidate symbols, then scan only those whose range
/// contains the target position. This eliminates linear search.
///
/// Thread safety: lock-free reads via an atomically-swapped RCU snapshot
/// (mirrors FileContentStore / ReferenceTracker). All read queries return by
/// value (SymbolID / std::vector<Symbol>), so callers need no lock and no pin.
/// Writers (index_file_symbols / remove_file / clear) clone-mutate-publish
/// under write_mu_, or mutate the bulk staging snapshot and publish once.
class SymbolLocationIndex {
  public:
    SymbolLocationIndex();

    /// Indexes all symbols in a file for position-based lookup.
    void index_file_symbols(FileID file_id,
                            std::span<const Symbol> symbols,
                            std::span<const EnhancedSymbol> enhanced_symbols);

    /// Finds the most specific symbol at a line/column position.
    /// Returns nullptr if no symbol contains the position.
    const Symbol* find_symbol_at_position(FileID file_id,
                                          int line, int column) const;

    /// Finds the symbol ID at a line/column position.
    /// Returns 0 if no symbol contains the position.
    SymbolID find_symbol_id_at_position(FileID file_id,
                                        int line, int column) const;

    /// Finds the symbol ID of the most-specific symbol whose line range spans
    /// `line` (column-agnostic). Returns 0 if none. Binary-searched O(log n);
    /// the line-only analogue of find_symbol_id_at_position for callers that
    /// only know a line (e.g. ReferenceTracker::get_symbol_at_line).
    SymbolID find_symbol_id_at_line(FileID file_id, int line) const;

    /// Returns all symbols in a file.
    std::vector<Symbol> get_file_symbols(FileID file_id) const;

    /// Removes all symbols for a specific file.
    void remove_file(FileID file_id);

    /// Removes all indexed symbols.
    void clear();

    /// Returns the number of indexed files.
    int file_count() const;

    /// Returns total number of indexed symbols across all files.
    int total_symbols() const;

    /// Opens (true) / closes (false) a bulk-index window: writes accumulate in
    /// a private staging snapshot and publish once on close, avoiding an
    /// O(files^2) whole-map clone per index_file_symbols during a full index.
    void set_bulk_indexing(bool enabled);

  private:
    /// Immutable read-side state, swapped atomically (RCU).
    struct Snapshot {
        absl::flat_hash_map<FileID, FileSymbolMap> locations;
    };

    AtomicSharedPtr<const Snapshot> snapshot_;
    mutable std::mutex write_mu_;
    std::shared_ptr<Snapshot> staging_;

    std::shared_ptr<const Snapshot> load_snapshot() const;
    template <class Fn>
    void write_snapshot(Fn&& fn);

    /// Finds the best-matching positioned symbol near a line/column.
    const PositionedSymbol* find_best_match(const FileSymbolMap& file_map,
                                            int line, int column) const;
};

}  // namespace lci
