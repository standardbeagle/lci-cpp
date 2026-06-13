#include <lci/core/symbol_store.h>

#include <algorithm>
#include <cstddef>

namespace lci {

// ---------------------------------------------------------------------------
// SymbolStore
// ---------------------------------------------------------------------------

SymbolStore::SymbolStore(int expected_size) {
    if (expected_size > 0) {
        auto cap = static_cast<size_t>(expected_size);
        data_.reserve(cap);
        reverse_index_.reserve(cap);
        index_.reserve(cap * 2);
    }
}

const EnhancedSymbol* SymbolStore::get(SymbolID id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return nullptr;
    return &data_[static_cast<size_t>(it->second)];
}

EnhancedSymbol* SymbolStore::get_mutable(SymbolID id) {
    auto it = index_.find(id);
    if (it == index_.end()) return nullptr;
    return &data_[static_cast<size_t>(it->second)];
}

void SymbolStore::set(SymbolID id, EnhancedSymbol symbol) {
    auto it = index_.find(id);
    if (it != index_.end()) {
        auto idx = static_cast<size_t>(it->second);
        update_stats_remove(data_[idx]);
        remove_from_secondary_indices(id, data_[idx]);
        data_[idx] = std::move(symbol);
        update_stats_add(data_[idx]);
        add_to_secondary_indices(id, data_[idx]);
        return;
    }

    index_[id] = static_cast<int>(data_.size());
    data_.push_back(std::move(symbol));
    reverse_index_.push_back(id);
    update_stats_add(data_.back());
    add_to_secondary_indices(id, data_.back());
}

bool SymbolStore::remove(SymbolID id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;

    auto idx = static_cast<size_t>(it->second);
    auto last_idx = data_.size() - 1;
    SymbolID last_id = reverse_index_[last_idx];

    update_stats_remove(data_[idx]);
    remove_from_secondary_indices(id, data_[idx]);

    if (idx != last_idx) {
        data_[idx] = std::move(data_[last_idx]);
        reverse_index_[idx] = last_id;
        index_[last_id] = static_cast<int>(idx);
    }

    data_.pop_back();
    reverse_index_.pop_back();
    index_.erase(it);

    return true;
}

int SymbolStore::size() const {
    return static_cast<int>(data_.size());
}

std::span<const EnhancedSymbol> SymbolStore::get_all() const {
    return data_;
}

std::vector<SymbolID> SymbolStore::get_ids() const {
    return {reverse_index_.begin(), reverse_index_.end()};
}

void SymbolStore::clear() {
    data_.clear();
    index_.clear();
    reverse_index_.clear();
    symbols_by_file_.clear();
    symbols_by_name_.clear();
    stats_ = {};
}

int SymbolStore::capacity() const {
    return static_cast<int>(data_.capacity());
}

std::span<const SymbolID> SymbolStore::get_symbols_by_file(FileID file_id) const {
    auto it = symbols_by_file_.find(file_id);
    if (it == symbols_by_file_.end()) return {};
    return it->second;
}

std::span<const SymbolID> SymbolStore::get_symbols_by_name(std::string_view name) const {
    auto it = symbols_by_name_.find(name);
    if (it == symbols_by_name_.end()) return {};
    return it->second;
}

std::vector<SymbolID> SymbolStore::get_entry_points() const {
    std::vector<SymbolID> result;
    for (size_t i = 0; i < data_.size(); ++i) {
        const auto& sym = data_[i];
        if (!sym.is_exported) continue;
        if (sym.symbol.type == SymbolType::Function ||
            sym.symbol.type == SymbolType::Method) {
            result.push_back(reverse_index_[i]);
        }
    }
    return result;
}

std::vector<SymbolID> SymbolStore::get_top_symbols(int limit) const {
    struct Ranked {
        SymbolID id;
        int ref_count;
    };

    std::vector<Ranked> ranked;
    ranked.reserve(data_.size());
    for (size_t i = 0; i < data_.size(); ++i) {
        int refs = static_cast<int>(data_[i].incoming_refs.size());
        ranked.push_back({reverse_index_[i], refs});
    }

    auto n = static_cast<size_t>(std::min(limit, static_cast<int>(ranked.size())));
    std::partial_sort(ranked.begin(), ranked.begin() + static_cast<ptrdiff_t>(n),
                      ranked.end(),
                      [](const Ranked& a, const Ranked& b) {
                          return a.ref_count > b.ref_count;
                      });

    std::vector<SymbolID> result;
    result.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        result.push_back(ranked[i].id);
    }
    return result;
}

const SymbolStoreStats& SymbolStore::stats() const {
    return stats_;
}

void SymbolStore::rebuild_indices() {
    symbols_by_file_.clear();
    symbols_by_name_.clear();
    stats_ = {};

    for (size_t i = 0; i < data_.size(); ++i) {
        SymbolID id = reverse_index_[i];
        update_stats_add(data_[i]);
        add_to_secondary_indices(id, data_[i]);
    }
}

void SymbolStore::update_stats_add(const EnhancedSymbol& sym) {
    ++stats_.total_symbols;
    switch (sym.symbol.type) {
        case SymbolType::Function: ++stats_.total_functions; break;
        case SymbolType::Method: ++stats_.total_methods; break;
        case SymbolType::Class: ++stats_.total_classes; break;
        case SymbolType::Variable: ++stats_.total_variables; break;
        case SymbolType::Constant: ++stats_.total_constants; break;
        default: break;
    }
}

void SymbolStore::update_stats_remove(const EnhancedSymbol& sym) {
    --stats_.total_symbols;
    switch (sym.symbol.type) {
        case SymbolType::Function: --stats_.total_functions; break;
        case SymbolType::Method: --stats_.total_methods; break;
        case SymbolType::Class: --stats_.total_classes; break;
        case SymbolType::Variable: --stats_.total_variables; break;
        case SymbolType::Constant: --stats_.total_constants; break;
        default: break;
    }
}

void SymbolStore::add_to_secondary_indices(SymbolID id,
                                           const EnhancedSymbol& sym) {
    symbols_by_file_[sym.symbol.file_id].push_back(id);
    symbols_by_name_[sym.symbol.name].push_back(id);

    if (symbols_by_file_[sym.symbol.file_id].size() == 1) {
        ++stats_.files_indexed;
    }
}

void SymbolStore::remove_from_secondary_indices(SymbolID id,
                                                const EnhancedSymbol& sym) {
    auto erase_id = [id](std::vector<SymbolID>& vec) {
        vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
    };

    auto file_it = symbols_by_file_.find(sym.symbol.file_id);
    if (file_it != symbols_by_file_.end()) {
        erase_id(file_it->second);
        if (file_it->second.empty()) {
            symbols_by_file_.erase(file_it);
            --stats_.files_indexed;
        }
    }

    auto name_it = symbols_by_name_.find(sym.symbol.name);
    if (name_it != symbols_by_name_.end()) {
        erase_id(name_it->second);
        if (name_it->second.empty()) {
            symbols_by_name_.erase(name_it);
        }
    }
}

// ---------------------------------------------------------------------------
// SymbolLocationIndex
// ---------------------------------------------------------------------------

SymbolLocationIndex::SymbolLocationIndex() {
    snapshot_.store(std::make_shared<const Snapshot>(),
                    std::memory_order_release);
}

std::shared_ptr<const SymbolLocationIndex::Snapshot>
SymbolLocationIndex::load_snapshot() const {
    return snapshot_.load(std::memory_order_acquire);
}

template <class Fn>
void SymbolLocationIndex::write_snapshot(Fn&& fn) {
    std::lock_guard<std::mutex> lk(write_mu_);
    if (staging_) {
        fn(*staging_);
        return;
    }
    auto next = std::make_shared<Snapshot>(
        *snapshot_.load(std::memory_order_acquire));
    fn(*next);
    snapshot_.store(std::move(next), std::memory_order_release);
}

void SymbolLocationIndex::set_bulk_indexing(bool enabled) {
    std::lock_guard<std::mutex> lk(write_mu_);
    if (enabled) {
        if (!staging_) {
            staging_ = std::make_shared<Snapshot>(
                *snapshot_.load(std::memory_order_acquire));
        }
    } else if (staging_) {
        snapshot_.store(std::move(staging_), std::memory_order_release);
        staging_ = nullptr;
    }
}

void SymbolLocationIndex::index_file_symbols(
    FileID file_id,
    std::span<const Symbol> symbols,
    std::span<const EnhancedSymbol> enhanced_symbols) {

    FileSymbolMap file_map;
    file_map.sorted_symbols.reserve(symbols.size());

    for (size_t i = 0; i < symbols.size(); ++i) {
        const auto& sym = symbols[i];

        SymbolID symbol_id = 0;
        if (i < enhanced_symbols.size()) {
            symbol_id = enhanced_symbols[i].id;
        } else {
            symbol_id = (static_cast<uint64_t>(sym.line) << 32) |
                        (static_cast<uint64_t>(sym.column) << 16) |
                        static_cast<uint64_t>(i);
        }

        file_map.sorted_symbols.push_back({
            .symbol = sym,
            .symbol_id = symbol_id,
            .start = {sym.line, sym.column},
            .end = {sym.end_line, sym.end_column},
        });
    }

    // Sort by start line, then by span size (smallest first for specificity).
    std::sort(file_map.sorted_symbols.begin(), file_map.sorted_symbols.end(),
              [](const PositionedSymbol& a, const PositionedSymbol& b) {
                  if (a.start.line != b.start.line) return a.start.line < b.start.line;
                  int a_size = (a.end.line - a.start.line) * 1000 + (a.end.column - a.start.column);
                  int b_size = (b.end.line - b.start.line) * 1000 + (b.end.column - b.start.column);
                  return a_size < b_size;
              });

    write_snapshot([&](Snapshot& s) {
        s.locations[file_id] = std::move(file_map);
    });
}

const Symbol* SymbolLocationIndex::find_symbol_at_position(
    FileID file_id, int line, int column) const {

    auto snap = load_snapshot();
    auto it = snap->locations.find(file_id);
    if (it == snap->locations.end()) return nullptr;

    const auto* match = find_best_match(it->second, line, column);
    if (match != nullptr) return &match->symbol;
    return nullptr;
}

SymbolID SymbolLocationIndex::find_symbol_id_at_position(
    FileID file_id, int line, int column) const {

    auto snap = load_snapshot();
    auto it = snap->locations.find(file_id);
    if (it == snap->locations.end()) return 0;

    const auto* match = find_best_match(it->second, line, column);
    if (match != nullptr) return match->symbol_id;
    return 0;
}

SymbolID SymbolLocationIndex::find_symbol_id_at_line(
    FileID file_id, int line) const {

    auto snap = load_snapshot();
    auto it = snap->locations.find(file_id);
    if (it == snap->locations.end()) return 0;

    const auto& syms = it->second.sorted_symbols;

    // sorted_symbols is ordered by start line, then span (smallest first).
    // All candidates have start_line <= line; binary-search the upper bound,
    // then take the first (smallest-span, most-specific) whose end_line >= line.
    // Column-agnostic: callers only know a line.
    auto upper = std::upper_bound(
        syms.begin(), syms.end(), line,
        [](int target_line, const PositionedSymbol& ps) {
            return target_line < ps.start.line;
        });

    const PositionedSymbol* best = nullptr;
    int best_size = -1;
    for (auto sit = syms.begin(); sit != upper; ++sit) {
        if (sit->end.line < line) continue;
        int sym_size = (sit->end.line - sit->start.line) * 1000 +
                       (sit->end.column - sit->start.column);
        if (best == nullptr || sym_size < best_size) {
            best = &(*sit);
            best_size = sym_size;
        }
    }

    return best != nullptr ? best->symbol_id : 0;
}

std::vector<Symbol> SymbolLocationIndex::get_file_symbols(FileID file_id) const {
    auto snap = load_snapshot();
    auto it = snap->locations.find(file_id);
    if (it == snap->locations.end()) return {};

    std::vector<Symbol> result;
    result.reserve(it->second.sorted_symbols.size());
    for (const auto& ps : it->second.sorted_symbols) {
        result.push_back(ps.symbol);
    }
    return result;
}

void SymbolLocationIndex::remove_file(FileID file_id) {
    write_snapshot([&](Snapshot& s) { s.locations.erase(file_id); });
}

void SymbolLocationIndex::clear() {
    std::lock_guard<std::mutex> lk(write_mu_);
    if (staging_) {
        staging_->locations.clear();
    } else {
        snapshot_.store(std::make_shared<const Snapshot>(),
                        std::memory_order_release);
    }
}

int SymbolLocationIndex::file_count() const {
    return static_cast<int>(load_snapshot()->locations.size());
}

int SymbolLocationIndex::total_symbols() const {
    auto snap = load_snapshot();
    int count = 0;
    for (const auto& [fid, file_map] : snap->locations) {
        count += static_cast<int>(file_map.sorted_symbols.size());
    }
    return count;
}

const PositionedSymbol* SymbolLocationIndex::find_best_match(
    const FileSymbolMap& file_map, int line, int column) const {

    const auto& syms = file_map.sorted_symbols;
    if (syms.empty()) return nullptr;

    // Binary search: find last symbol whose start line <= target line.
    // All symbols with start_line <= line are candidates if their end_line >= line.
    auto upper = std::upper_bound(
        syms.begin(), syms.end(), line,
        [](int target_line, const PositionedSymbol& ps) {
            return target_line < ps.start.line;
        });

    const PositionedSymbol* best = nullptr;
    int best_size = -1;

    // Scan backwards from upper bound through all candidates.
    for (auto it = syms.begin(); it != upper; ++it) {
        if (it->end.line < line) continue;
        if (line == it->start.line && column < it->start.column) continue;
        if (line == it->end.line && column > it->end.column) continue;

        int sym_size = (it->end.line - it->start.line) * 1000 +
                       (it->end.column - it->start.column);
        if (best == nullptr || sym_size < best_size) {
            best = &(*it);
            best_size = sym_size;
        }
    }

    return best;
}

}  // namespace lci
