#include <lci/core/reference_tracker.h>

#include <absl/container/inlined_vector.h>
#include <lci/path_classifier.h>  // canonical test/example/vendored tagging

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <string>

namespace lci {

namespace {
ReferenceTracker::LangFamily language_family_for_path(std::string_view path);
bool is_low_quality_path(const PathAttrRegistry& registry,
                         std::string_view path);
uint64_t dir_hash_of_path(std::string_view path);
}  // namespace

// FNV-1a constants for 64-bit hash.
static constexpr uint64_t kFnvOffset64 = 14695981039346656037ULL;
static constexpr uint64_t kFnvPrime64 = 1099511628211ULL;

// ---------------------------------------------------------------------------
// TypeRelationships
// ---------------------------------------------------------------------------

bool TypeRelationships::has_relationships() const {
    return !implements.empty() || !implemented_by.empty() ||
           !extends.empty() || !extended_by.empty();
}

// ---------------------------------------------------------------------------
// ReferenceTracker::Snapshot - read-side query logic over frozen state
// ---------------------------------------------------------------------------

namespace {
/// Mutable twin of Snapshot::find_ref for write-side mutations. Returns
/// the slot even when tombstoned is false-only (id != 0 checked), same
/// decode as the read side.
StoredRef* find_mutable_ref(ReferenceTracker::Snapshot& s, uint64_t ref_id) {
    const auto file_id = static_cast<FileID>(ref_id >> 32);
    const auto local = static_cast<uint32_t>(ref_id & 0xFFFFFFFFu);
    if (local == 0) return nullptr;
    auto it = s.refs_by_file.find(file_id);
    if (it == s.refs_by_file.end()) return nullptr;
    auto& vec = it->second;
    if (local > vec.size()) return nullptr;
    StoredRef& r = vec[local - 1];
    return r.dead ? nullptr : &r;
}
}  // namespace

const StoredRef* ReferenceTracker::Snapshot::find_ref(uint64_t ref_id) const {
    const auto file_id = static_cast<FileID>(ref_id >> 32);
    const auto local = static_cast<uint32_t>(ref_id & 0xFFFFFFFFu);
    if (local == 0) return nullptr;
    auto it = refs_by_file.find(file_id);
    if (it == refs_by_file.end()) return nullptr;
    const auto& vec = it->second;
    if (local > vec.size()) return nullptr;
    const StoredRef& r = vec[local - 1];
    return r.dead ? nullptr : &r;
}

Reference ReferenceTracker::Snapshot::materialize_ref(
    FileID file_id, uint32_t local_id, const StoredRef& r) const {
    Reference out;
    out.id = (static_cast<uint64_t>(file_id) << 32) |
             static_cast<uint64_t>(local_id);
    out.source_symbol = r.source_symbol;
    out.target_symbol = r.target_symbol;
    out.file_id = file_id;
    out.line = r.line;
    out.column = r.column;
    out.type = r.type;
    out.strength = r.strength;
    out.ambiguous = r.ambiguous;
    out.foreign_receiver = r.foreign_receiver;
    out.type_position = r.type_position;
    out.call_arg_count = r.stored_arg_count();
    if (r.name_id < ref_names.size()) {
        out.referenced_name = ref_names[r.name_id];
    }
    return out;
}

int ReferenceTracker::Snapshot::count_unresolved_calls(
    std::string_view name) const {
    if (name.empty()) return 0;
    // Interned pool: collect the ids whose spelling is `name` or ends in
    // ".name" (typed-receiver qualifications), then count live unresolved
    // Call refs carrying one of them. Two linear passes; query paths only.
    absl::flat_hash_set<uint32_t> ids;
    for (size_t i = 0; i < ref_names.size(); ++i) {
        const std::string& n = ref_names[i];
        if (n == name ||
            (n.size() > name.size() + 1 &&
             n[n.size() - name.size() - 1] == '.' &&
             std::string_view(n).substr(n.size() - name.size()) == name)) {
            ids.insert(static_cast<uint32_t>(i));
        }
    }
    if (ids.empty()) return 0;
    int count = 0;
    for (const auto& [fid, vec] : refs_by_file) {
        for (const auto& r : vec) {
            if (r.dead || r.type != ReferenceType::Call) continue;
            if (r.target_symbol != 0) continue;
            if (ids.contains(r.name_id)) ++count;
        }
    }
    return count;
}

size_t ReferenceTracker::Snapshot::live_ref_count() const {
    size_t n = 0;
    for (const auto& [fid, vec] : refs_by_file) {
        for (const auto& r : vec) {
            if (!r.dead) ++n;
        }
    }
    return n;
}

std::vector<Reference> ReferenceTracker::Snapshot::get_references_by_id(
    std::span<const uint64_t> ref_ids) const {
    std::vector<Reference> result;
    result.reserve(ref_ids.size());
    for (uint64_t id : ref_ids) {
        if (const StoredRef* r = find_ref(id)) {
            result.push_back(materialize_ref(
                static_cast<FileID>(id >> 32),
                static_cast<uint32_t>(id & 0xFFFFFFFFu), *r));
        }
    }
    return result;
}

std::vector<ReferenceTracker::Snapshot::SymbolHandle>
ReferenceTracker::Snapshot::find_symbols_by_name(std::string_view name) const {
    auto ids = symbols.get_symbols_by_name(name);
    std::vector<SymbolHandle> result;
    result.reserve(ids.size());
    for (SymbolID id : ids) {
        if (const auto* s = symbols.get(id)) {
            result.emplace_back(shared_from_this(), s);
        }
    }
    return result;
}

std::vector<Reference> ReferenceTracker::Snapshot::get_symbol_references(
    SymbolID symbol_id, std::string_view direction) const {
    std::vector<uint64_t> ref_ids;

    const bool want_incoming =
        direction == "incoming" || direction == "both" ||
        (direction != "outgoing");
    const bool want_outgoing =
        direction == "outgoing" || direction == "both" ||
        (direction != "incoming");

    if (want_incoming) {
        if (auto it = incoming_refs.find(symbol_id);
            it != incoming_refs.end()) {
            ref_ids.insert(ref_ids.end(), it->second.begin(),
                           it->second.end());
        }
    }
    if (want_outgoing) {
        if (auto it = outgoing_refs.find(symbol_id);
            it != outgoing_refs.end()) {
            ref_ids.insert(ref_ids.end(), it->second.begin(),
                           it->second.end());
        }
    }

    return get_references_by_id(ref_ids);
}

ReferenceTracker::Snapshot::SymbolHandle
ReferenceTracker::Snapshot::get_enhanced_symbol(
    SymbolID symbol_id) const {
    const auto* symbol = symbols.get(symbol_id);
    if (symbol == nullptr) return {};
    return {shared_from_this(), symbol};
}

std::vector<ReferenceTracker::Snapshot::SymbolHandle>
ReferenceTracker::Snapshot::get_file_enhanced_symbols(FileID file_id) const {
    auto ids = symbols.get_symbols_by_file(file_id);
    std::vector<SymbolHandle> result;
    result.reserve(ids.size());
    for (SymbolID id : ids) {
        if (const auto* s = symbols.get(id)) {
            result.emplace_back(shared_from_this(), s);
        }
    }
    return result;
}

ReferenceTracker::Snapshot::SymbolHandle
ReferenceTracker::Snapshot::find_symbol_by_name(
    std::string_view name) const {
    auto ids = symbols.get_symbols_by_name(name);
    if (ids.empty()) return nullptr;
    return get_enhanced_symbol(ids[0]);
}

ReferenceTracker::Snapshot::SymbolHandle
ReferenceTracker::Snapshot::find_symbol_by_file_and_name(
    FileID file_id, std::string_view name) const {
    auto ids = symbols.get_symbols_by_name(name);
    for (SymbolID id : ids) {
        if (const auto* s = symbols.get(id)) {
            if (s->symbol.file_id == file_id) {
                return {shared_from_this(), s};
            }
        }
    }
    return nullptr;
}

ReferenceTracker::Snapshot::SymbolHandle
ReferenceTracker::Snapshot::get_symbol_at_line(
    FileID file_id, int line) const {
    auto file_syms = symbols.get_symbols_by_file(file_id);
    for (SymbolID sid : file_syms) {
        if (const auto* s = symbols.get(sid)) {
            if (s->symbol.line <= line && line <= s->symbol.end_line) {
                return {shared_from_this(), s};
            }
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// ReferenceTracker
// ---------------------------------------------------------------------------

ReferenceTracker::ReferenceTracker(SymbolLocationIndex* location_index)
    : symbol_location_index_(location_index) {
    snapshot_.store(std::make_shared<const Snapshot>(),
                    std::memory_order_release);
}

std::shared_ptr<const ReferenceTracker::Snapshot>
ReferenceTracker::load_snapshot() const {
    return snapshot_.load(std::memory_order_acquire);
}

template <class Fn>
void ReferenceTracker::write_snapshot(Fn&& fn) {
    std::lock_guard<std::mutex> lk(write_mu_);
    if (staging_) {
        // Bulk window: mutate the private unpublished snapshot in place;
        // a single publish happens in set_bulk_indexing(false).
        fn(*staging_);
        return;
    }
    auto next = std::make_shared<Snapshot>(
        *snapshot_.load(std::memory_order_acquire));
    fn(*next);
    snapshot_.store(std::move(next), std::memory_order_release);
}

void ReferenceTracker::set_bulk_indexing(bool enabled) {
    std::lock_guard<std::mutex> lk(write_mu_);
    bulk_indexing.store(enabled ? 1 : 0, std::memory_order_release);
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

void ReferenceTracker::clear() {
    std::lock_guard<std::mutex> lk(write_mu_);
    import_data_.clear();
    reference_cache_.clear();
    // Reset the name intern table WITH the pool (a fresh snapshot has an
    // empty pool; the table must never point past it). The pass-end memo
    // clear below deliberately does NOT touch this table -- pool ids stay
    // valid across passes, and keeping the table is what makes re-indexed
    // files reuse existing pool entries instead of appending duplicates.
    ref_name_ids_.clear();
    scope_chain_cache_.clear();
    file_resolution_meta_.clear();
    next_symbol_id_ = 1;
    next_ref_id_ = 1;
    import_resolver_.clear();
    if (staging_) {
        staging_ = std::make_shared<Snapshot>();
    } else {
        snapshot_.store(std::make_shared<const Snapshot>(),
                        std::memory_order_release);
    }
}

// -- File processing ---------------------------------------------------------

std::vector<EnhancedSymbol> ReferenceTracker::process_file(
    FileID file_id, std::string_view path,
    std::span<const Symbol> symbols,
    std::span<const Reference> references,
    std::span<const ScopeInfo> scopes) {

    std::vector<EnhancedSymbol> enhanced;
    enhanced.reserve(symbols.size());

    file_resolution_meta_[file_id] = FileResolutionMeta{
        language_family_for_path(path), is_low_quality_path(attr_registry(), path),
        dir_hash_of_path(path)};

    write_snapshot([&](Snapshot& s) {
        s.scopes_by_file[file_id].assign(scopes.begin(), scopes.end());

        for (const auto& sym : symbols) {
            SymbolID id = next_symbol_id_++;
            // scope_chain lives ONLY on the EnhancedSymbol. A parallel
            // symbol_scopes map used to hold a second deep copy (~9 MB of
            // strings on packages/next alone) with zero readers.
            auto scope_chain = build_symbol_scope_chain(sym, scopes);

            Symbol sm = sym;
            sm.file_id = file_id;

            // Declared visibility beats the name heuristic: a `private
            // function` is not API whatever its spelling (before the
            // extractor wrote visibility, every PHP/C#/Java private method
            // counted as exported).
            bool is_exported =
                sm.visibility == SymbolVisibility::Private ||
                        sm.visibility == SymbolVisibility::Protected ||
                        sm.visibility == SymbolVisibility::Internal
                    ? false
                    : compute_is_exported(path, sm.name);

            EnhancedSymbol es;
            es.symbol = std::move(sm);
            es.id = id;
            es.scope_chain = std::move(scope_chain);
            es.is_exported = is_exported;
            // Mirror the extractor's per-symbol parameter count onto the
            // enhanced field every params sort/filter reads (this field had
            // no writer at all before — sorts ranked all-zeros).
            es.parameter_count = es.symbol.parameter_count;

            s.symbols.set(id, es);

            enhanced.push_back(std::move(es));
        }

        // Store references for later processing, in the slim storage
        // shape. Local id = position + 1, assigned by construction (the
        // caller's ids are untrusted -- tests pass id 0); id and file_id
        // are never stored, they ARE the slot address. Names intern into
        // the snapshot pool: every repeat of a name across the corpus
        // costs 4 bytes.
        {
            auto& vec = s.refs_by_file[file_id];
            vec.clear();
            vec.reserve(references.size());
            for (const auto& ref : references) {
                StoredRef r;
                r.source_symbol = ref.source_symbol;
                r.target_symbol = ref.target_symbol;
                r.line = ref.line;
                r.column = ref.column;
                r.type = ref.type;
                r.strength = ref.strength;
                r.ambiguous = ref.ambiguous;
                r.foreign_receiver = ref.foreign_receiver;
                r.type_position = ref.type_position;
                r.arg_count_p1 = StoredRef::encode_arg_count(
                    ref.call_arg_count) & 0xF;
                r.name_id = intern_ref_name(s, ref.referenced_name);
                vec.push_back(r);
            }
        }
    });

    return enhanced;
}

namespace {
std::string derive_receiver_type(const EnhancedSymbol& es);
}  // namespace

void ReferenceTracker::apply_enrichment(
    std::span<const EnhancedSymbol> enriched) {
    if (enriched.empty()) return;
    write_snapshot([&](Snapshot& s) {
        for (const auto& es : enriched) {
            if (es.receiver_type.empty()) {
                EnhancedSymbol copy = es;
                copy.receiver_type = derive_receiver_type(es);
                s.symbols.set(copy.id, copy);
            } else {
                s.symbols.set(es.id, es);
            }
        }
    });
}

void ReferenceTracker::process_file_imports(
    FileID file_id, std::string_view file_path, std::string_view content) {
    auto data = import_resolver_.extract_file_imports(file_id, file_path,
                                                      content);
    if (!data.bindings.empty()) {
        import_data_.push_back(std::move(data));
    }
}

void ReferenceTracker::process_all_references() {
    import_resolver_.build_import_graph(import_data_);
    import_data_.clear();

    write_snapshot([&](Snapshot& s) {
        s.incoming_refs.clear();
        s.outgoing_refs.clear();

        // Get symbol IDs by file for resolution.
        absl::flat_hash_map<FileID, std::vector<SymbolID>> symbols_by_file;
        s.symbols.range([&](SymbolID id, const EnhancedSymbol& es) {
            symbols_by_file[es.symbol.file_id].push_back(id);
            return true;
        });

        // refs_by_file is a flat_hash_map, whose iteration order is randomized
        // per process. That order is user-visible (structure `used_by`,
        // relationships, imports), so walk the owning files in sorted id order;
        // within a file the slots are already in deterministic order.
        std::vector<FileID> ordered_ref_files;
        ordered_ref_files.reserve(s.refs_by_file.size());
        for (const auto& [owner_fid, ref_vec] : s.refs_by_file) {
            ordered_ref_files.push_back(owner_fid);
        }
        std::sort(ordered_ref_files.begin(), ordered_ref_files.end());

        for (FileID owner_fid : ordered_ref_files) {
          auto& ref_vec = s.refs_by_file[owner_fid];
          for (uint32_t idx = 0; idx < ref_vec.size(); ++idx) {
            StoredRef& ref = ref_vec[idx];
            if (ref.dead) continue;
            const uint64_t ref_id = make_global_ref_id(owner_fid, idx + 1);
            const std::string_view name =
                ref.name_id < s.ref_names.size()
                    ? std::string_view(s.ref_names[ref.name_id])
                    : std::string_view{};
            SymbolID source_id = ref.source_symbol;
            SymbolID target_id = ref.target_symbol;

            if (source_id == 0) {
                source_id = find_symbol_at_location(s, owner_fid, ref.line,
                                                     ref.column);
                if (source_id != 0) ref.source_symbol = source_id;
            }
            if (target_id == 0) {
                auto it = symbols_by_file.find(owner_fid);
                std::span<const SymbolID> file_syms;
                if (it != symbols_by_file.end()) {
                    file_syms = it->second;
                }
                target_id = resolve_reference_target(s, ref, owner_fid, name,
                                                     file_syms);
                if (target_id != 0) ref.target_symbol = target_id;
            }

            if (source_id != 0) {
                s.outgoing_refs[source_id].push_back(ref_id);
            }
            if (target_id != 0) {
                s.incoming_refs[target_id].push_back(ref_id);
            }
          }
        }

        update_reference_stats(s);
    });

    // Both memos are intra-pass: resolve_reference_target (the sole
    // reference_cache_ user) only runs inside this function, and the
    // scope-chain memo's reuse window is the bulk enrichment that just
    // ended. Dropping them here reclaims the memory (the scope memo holds
    // FULL ScopeInfo vector copies) and closes a staleness hazard --
    // reference_cache_ had no per-entry validity guard, so a cached
    // SymbolID could outlive its symbol across passes. Each pass now
    // starts from truth and rebuilds its own memo.
    {
        std::lock_guard<std::mutex> lk(write_mu_);
        reference_cache_.clear();
        scope_chain_cache_.clear();
    }
}

void ReferenceTracker::remove_file(FileID file_id) {
    file_resolution_meta_.erase(file_id);
    write_snapshot([&](Snapshot& s) {
        auto file_syms = s.symbols.get_symbols_by_file(file_id);
        std::vector<SymbolID> ids(file_syms.begin(), file_syms.end());

        for (SymbolID sym_id : ids) {
            // Remove outgoing references.
            if (auto it = s.outgoing_refs.find(sym_id);
                it != s.outgoing_refs.end()) {
                for (uint64_t ref_id : it->second) {
                    if (StoredRef* r = find_mutable_ref(s, ref_id)) {
                        if (r->target_symbol != 0) {
                            remove_from_incoming_refs(s, r->target_symbol,
                                                      ref_id);
                        }
                        r->dead = true;
                    }
                }
                s.outgoing_refs.erase(it);
            }

            // Remove incoming references.
            if (auto it = s.incoming_refs.find(sym_id);
                it != s.incoming_refs.end()) {
                for (uint64_t ref_id : it->second) {
                    if (const StoredRef* r = find_mutable_ref(s, ref_id)) {
                        if (r->source_symbol != 0) {
                            remove_from_outgoing_refs(s, r->source_symbol,
                                                      ref_id);
                        }
                    }
                }
                s.incoming_refs.erase(it);
            }

            s.symbols.remove(sym_id);
        }

        // The file's own reference storage goes wholesale -- this is the
        // payoff of the per-file store: no per-reference hash erases, and
        // any tombstones the symbol walk above left in it die with it.
        s.refs_by_file.erase(file_id);
        s.scopes_by_file.erase(file_id);
    });

    import_resolver_.remove_file(file_id);
}

// -- Query methods -----------------------------------------------------------

std::vector<Reference> ReferenceTracker::get_symbol_references(
    SymbolID symbol_id, std::string_view direction) const {
    return load_snapshot()->get_symbol_references(symbol_id, direction);
}

std::vector<Reference> ReferenceTracker::get_file_references(
    FileID file_id) const {
    auto snap = load_snapshot();
    auto file_syms = snap->symbols.get_symbols_by_file(file_id);
    std::vector<uint64_t> ref_ids;

    for (SymbolID sym_id : file_syms) {
        if (auto it = snap->outgoing_refs.find(sym_id);
            it != snap->outgoing_refs.end()) {
            ref_ids.insert(ref_ids.end(), it->second.begin(),
                           it->second.end());
        }
        if (auto it = snap->incoming_refs.find(sym_id);
            it != snap->incoming_refs.end()) {
            ref_ids.insert(ref_ids.end(), it->second.begin(),
                           it->second.end());
        }
    }

    return snap->get_references_by_id(ref_ids);
}

std::vector<Reference> ReferenceTracker::get_all_references() const {
    auto snap = load_snapshot();
    std::vector<Reference> out;
    out.reserve(snap->live_ref_count());
    snap->for_each_live_ref([&](FileID fid, uint32_t local,
                                const StoredRef& ref) {
        out.push_back(snap->materialize_ref(fid, local, ref));
    });
    return out;
}

// -- Type relationship queries -----------------------------------------------

std::vector<SymbolID> ReferenceTracker::get_implementors(
    SymbolID interface_id) const {
    return get_symbols_by_ref_type(interface_id, true,
                                    ReferenceType::Implements);
}

std::vector<SymbolID> ReferenceTracker::get_implemented_interfaces(
    SymbolID type_id) const {
    return get_symbols_by_ref_type(type_id, false,
                                    ReferenceType::Implements);
}

std::vector<SymbolID> ReferenceTracker::get_base_types(
    SymbolID type_id) const {
    return get_symbols_by_ref_type(type_id, false, ReferenceType::Extends);
}

std::vector<SymbolID> ReferenceTracker::get_derived_types(
    SymbolID base_id) const {
    return get_symbols_by_ref_type(base_id, true, ReferenceType::Extends);
}

TypeRelationships ReferenceTracker::get_type_relationships(
    SymbolID symbol_id) const {
    return TypeRelationships{
        .implements = get_implemented_interfaces(symbol_id),
        .implemented_by = get_implementors(symbol_id),
        .extends = get_base_types(symbol_id),
        .extended_by = get_derived_types(symbol_id),
    };
}

// -- Call graph utilities ----------------------------------------------------

std::vector<std::string> ReferenceTracker::get_callee_names(
    SymbolID symbol_id) const {
    auto snap = load_snapshot();
    auto refs = snap->get_symbol_references(symbol_id, "outgoing");
    absl::flat_hash_map<std::string, bool> seen;
    std::vector<std::string> result;
    for (const auto& ref : refs) {
        if (ref.type == ReferenceType::Call && !ref.referenced_name.empty()) {
            if (!seen.contains(ref.referenced_name)) {
                seen[ref.referenced_name] = true;
                result.push_back(ref.referenced_name);
            }
        }
    }
    return result;
}

std::vector<std::string> ReferenceTracker::get_caller_names(
    SymbolID symbol_id) const {
    auto snap = load_snapshot();
    auto refs = snap->get_symbol_references(symbol_id, "incoming");
    absl::flat_hash_map<SymbolID, bool> seen;
    std::vector<std::string> result;
    for (const auto& ref : refs) {
        if (ref.type == ReferenceType::Call && ref.source_symbol != 0) {
            if (!seen.contains(ref.source_symbol)) {
                if (const auto* src = snap->symbols.get(ref.source_symbol)) {
                    seen[ref.source_symbol] = true;
                    // Anonymous callers (closures / func literals) have an empty
                    // symbol name; surface a readable label instead of a blank
                    // entry so a list of callers is legible.
                    result.push_back(src->symbol.name.empty()
                                         ? "<anonymous>"
                                         : src->symbol.name);
                }
            }
        }
    }
    return result;
}

std::vector<SymbolID> ReferenceTracker::get_callee_symbols(
    SymbolID symbol_id) const {
    auto refs = load_snapshot()->get_symbol_references(symbol_id, "outgoing");
    absl::flat_hash_map<SymbolID, bool> seen;
    std::vector<SymbolID> result;
    for (const auto& ref : refs) {
        if (ref.type == ReferenceType::Call && ref.target_symbol != 0) {
            if (!seen.contains(ref.target_symbol)) {
                seen[ref.target_symbol] = true;
                result.push_back(ref.target_symbol);
            }
        }
    }
    return result;
}

std::vector<SymbolID> ReferenceTracker::get_outgoing_target_symbols(
    SymbolID symbol_id) const {
    auto refs = load_snapshot()->get_symbol_references(symbol_id, "outgoing");
    std::vector<SymbolID> result;
    result.reserve(refs.size());
    for (const auto& ref : refs) {
        if (ref.target_symbol != 0) result.push_back(ref.target_symbol);
    }
    return result;
}

std::vector<SymbolID> ReferenceTracker::get_caller_symbols(
    SymbolID symbol_id) const {
    auto refs = load_snapshot()->get_symbol_references(symbol_id, "incoming");
    absl::flat_hash_map<SymbolID, bool> seen;
    std::vector<SymbolID> result;
    for (const auto& ref : refs) {
        if (ref.type == ReferenceType::Call && ref.source_symbol != 0) {
            if (!seen.contains(ref.source_symbol)) {
                seen[ref.source_symbol] = true;
                result.push_back(ref.source_symbol);
            }
        }
    }
    return result;
}

FunctionTreeNode ReferenceTracker::build_function_tree(
    SymbolID symbol_id, int max_depth) const {
    auto snap = load_snapshot();
    absl::flat_hash_map<SymbolID, bool> visited;
    return build_tree_node(*snap, symbol_id, 0, max_depth, visited);
}

// -- Statistics ---------------------------------------------------------------

ReferenceStats ReferenceTracker::get_reference_stats() const {
    return load_snapshot()->stats;
}

bool ReferenceTracker::has_relationships() const {
    auto snap = load_snapshot();
    return !snap->incoming_refs.empty() || !snap->outgoing_refs.empty() ||
           snap->stats.total_references > 0;
}

// -- Internal helpers --------------------------------------------------------

void ReferenceTracker::remove_from_incoming_refs(Snapshot& s,
                                                  SymbolID symbol_id,
                                                  uint64_t ref_id) {
    auto it = s.incoming_refs.find(symbol_id);
    if (it == s.incoming_refs.end()) return;
    auto& refs = it->second;
    std::erase(refs, ref_id);
    if (refs.empty()) s.incoming_refs.erase(it);
}

void ReferenceTracker::remove_from_outgoing_refs(Snapshot& s,
                                                  SymbolID symbol_id,
                                                  uint64_t ref_id) {
    auto it = s.outgoing_refs.find(symbol_id);
    if (it == s.outgoing_refs.end()) return;
    auto& refs = it->second;
    std::erase(refs, ref_id);
    if (refs.empty()) s.outgoing_refs.erase(it);
}

uint64_t ReferenceTracker::make_global_ref_id(FileID file_id,
                                               uint32_t local_ref_id) {
    return (static_cast<uint64_t>(file_id) << 32) |
           static_cast<uint64_t>(local_ref_id);
}

bool ReferenceTracker::compute_is_exported(std::string_view path,
                                            std::string_view symbol_name) {
    if (symbol_name.empty()) return false;

    // Visibility rule is keyed by language family from the centralized table
    // (language_map.h) rather than a private extension list.
    switch (language_info_for_path(path).family) {
        case LangFamily::kGo:
            return std::isupper(static_cast<unsigned char>(symbol_name[0])) != 0;
        case LangFamily::kPython:
        case LangFamily::kRuby:
            return !symbol_name.starts_with('_');
        case LangFamily::kJsTs:
            return !symbol_name.starts_with('_') && !symbol_name.starts_with('#');
        default:
            // C/C++, Java, Kotlin, Rust, C#, PHP, Zig, and unknown: assume
            // exported.
            return true;
    }
}

uint32_t ReferenceTracker::intern_ref_name(Snapshot& s,
                                           std::string_view name) {
    if (auto it = ref_name_ids_.find(name); it != ref_name_ids_.end()) {
        return it->second;
    }
    const auto id = static_cast<uint32_t>(s.ref_names.size());
    s.ref_names.emplace_back(name);
    ref_name_ids_.emplace(std::string(name), id);
    return id;
}

ScopeChain ReferenceTracker::build_symbol_scope_chain(
    const Symbol& symbol, std::span<const ScopeInfo> scopes) {

    // Select the enclosing scopes first; the hash-cons key is the CONTENT
    // of this selection. Keying on the symbol's own line span (the first
    // cut) made sharing impossible for siblings on different lines --
    // measured 19% dedup at next.js scale where class members should
    // share almost fully.
    thread_local std::vector<const ScopeInfo*> selected;
    selected.clear();
    for (const auto& scope : scopes) {
        if (scope.start_line <= symbol.line &&
            (scope.end_line == 0 || scope.end_line >= symbol.line)) {
            selected.push_back(&scope);
        }
    }

    uint64_t h = kFnvOffset64;
    for (const ScopeInfo* sc : selected) {
        h ^= static_cast<uint64_t>(sc->start_line);
        h *= kFnvPrime64;
        h ^= static_cast<uint64_t>(sc->end_line);
        h *= kFnvPrime64;
        for (char c : sc->name) {
            h ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
            h *= kFnvPrime64;
        }
    }

    auto matches = [&](const ScopeChain& chain) {
        if (chain.size() != selected.size()) return false;
        for (size_t i = 0; i < selected.size(); ++i) {
            const ScopeInfo& a = chain[i];
            const ScopeInfo* b = selected[i];
            if (a.start_line != b->start_line || a.end_line != b->end_line ||
                a.name != b->name || a.full_path != b->full_path) {
                return false;
            }
        }
        return true;
    };

    if (auto it = scope_chain_cache_.find(h);
        it != scope_chain_cache_.end() && matches(it->second.scope_chain)) {
        return it->second.scope_chain;
    }

    auto chain = std::make_shared<std::vector<ScopeInfo>>();
    chain->reserve(selected.size());
    for (const ScopeInfo* sc : selected) chain->push_back(*sc);

    ScopeChain shared{std::move(chain)};
    scope_chain_cache_[h] = ScopeChainCacheEntry{.scope_chain = shared};
    return shared;
}


SymbolID ReferenceTracker::find_symbol_at_location(
    const Snapshot& s, FileID file_id, int line, int col) const {

    if (symbol_location_index_ != nullptr) {
        return symbol_location_index_->find_symbol_id_at_position(
            file_id, line, col);
    }

    // Fallback: linear scan.
    auto file_syms = s.symbols.get_symbols_by_file(file_id);
    for (SymbolID id : file_syms) {
        if (const auto* sym = s.symbols.get(id)) {
            if (sym->symbol.line <= line && sym->symbol.end_line >= line) {
                if (sym->symbol.line == line) {
                    if (col >= sym->symbol.column &&
                        col <= sym->symbol.end_column) {
                        return id;
                    }
                } else if (sym->symbol.line < line &&
                           sym->symbol.end_line > line) {
                    return id;
                }
            }
        }
    }
    return 0;
}

uint64_t ReferenceTracker::fnv1a_hash_name(std::string_view name) {
    uint64_t h = kFnvOffset64;
    for (char c : name) {
        h ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
        h *= kFnvPrime64;
    }
    return h;
}

namespace {
// Bare type name from a possibly-decorated receiver token: "*chi.Mux" -> "Mux".
std::string_view bare_type_name(std::string_view t) {
    size_t i = 0;
    while (i < t.size() && (t[i] == '*' || t[i] == '&')) ++i;
    t = t.substr(i);
    if (auto dot = t.rfind('.'); dot != std::string_view::npos)
        t = t.substr(dot + 1);
    return t;
}

// Go method-receiver type from a signature: "func (r *Mux) M(...)" -> "Mux".
std::string_view go_signature_receiver(std::string_view sig) {
    constexpr std::string_view kFunc = "func (";
    if (sig.rfind(kFunc, 0) != 0) return {};
    auto close = sig.find(')', kFunc.size());
    if (close == std::string_view::npos) return {};
    std::string_view recv = sig.substr(kFunc.size(), close - kFunc.size());
    if (auto sp = recv.rfind(' '); sp != std::string_view::npos)
        recv = recv.substr(sp + 1);  // drop the receiver var name
    return bare_type_name(recv);
}

/// The receiver_type WRITER the field never had: the /list-symbols
/// receiver filter compares against EnhancedSymbol::receiver_type, and
/// with no writer it silently matched nothing. Derivation mirrors the
/// matcher (symbol_matches_receiver_type): Go methods from the signature
/// receiver, class-language methods from the NEAREST enclosing
/// Class/Struct scope (chains are ordered outermost-first, so scan from
/// the back). Top-level symbols stay "" -- absence keeps its meaning.
std::string derive_receiver_type(const EnhancedSymbol& es) {
    if (auto recv = go_signature_receiver(es.signature); !recv.empty()) {
        return std::string(recv);
    }
    const auto& chain = es.scope_chain;
    for (size_t i = chain.size(); i > 0; --i) {
        const ScopeInfo& sc = chain[i - 1];
        if (sc.type == ScopeType::Class || sc.type == ScopeType::Struct) {
            return std::string(bare_type_name(sc.name));
        }
    }
    return {};
}

// Does this symbol's owning/receiver type equal `recv_type`? Matches Go
// receivers (parsed from the signature) and class-based languages (the
// enclosing class appears in scope_chain).
bool symbol_matches_receiver_type(const EnhancedSymbol& sym,
                                  std::string_view recv_type) {
    if (go_signature_receiver(sym.signature) == recv_type) return true;
    for (const auto& sc : sym.scope_chain) {
        if (bare_type_name(sc.name) == recv_type) return true;
    }
    return false;
}

// See LangFamily in the header for why families rather than exact languages.
// Delegates to the centralized extension table (language_map.h): the single
// source of truth for extension -> family, shared with every other site.
ReferenceTracker::LangFamily language_family_for_path(std::string_view path) {
    return language_info_for_path(path).family;
}

// A file that does not activate the Refs capability loses to library code
// when an ambiguous name has to be resolved without import evidence — tests,
// benchmarks, examples, vendored trees, generated emitters. The attribute
// registry is the single authority; this used to hold a private list
// ("benchmarks", "fixtures") on top of a builtin-only classifier, so a
// project's own `.lci.kdl` attributes never reached reference resolution.
// Runs once per file at process_file time, never on a read path.
bool is_low_quality_path(const PathAttrRegistry& registry,
                         std::string_view path) {
    PathClassifier classifier(registry);
    return !registry.activates(classifier.classify(path), Capability::Refs);
}

// Directory identity for package-proximity resolution (Go package = dir; most
// languages cluster a module per directory). Hash of the parent-directory
// prefix; files in the repo root share the empty-dir hash.
uint64_t dir_hash_of_path(std::string_view path) {
    auto slash = path.rfind('/');
    std::string_view dir =
        (slash == std::string_view::npos) ? std::string_view{}
                                          : path.substr(0, slash);
    uint64_t h = kFnvOffset64;
    for (char c : dir) {
        h ^= static_cast<unsigned char>(c);
        h *= kFnvPrime64;
    }
    return h;
}
}  // namespace

namespace {

/// Symbols a TYPE-position reference may resolve to.
bool is_type_like_symbol(SymbolType t) {
    switch (t) {
        case SymbolType::Class:
        case SymbolType::Struct:
        case SymbolType::Interface:
        case SymbolType::Type:
        case SymbolType::Enum:
        case SymbolType::Record:
        case SymbolType::Delegate:
            return true;
        default:
            return false;
    }
}

}  // namespace

SymbolID ReferenceTracker::resolve_reference_target(
    const Snapshot& s, const StoredRef& ref, FileID owner_fid,
    std::string_view full_name, std::span<const SymbolID> file_symbol_ids) {

    if (full_name.empty()) return 0;

    // (file_id, FULL 64-bit name hash). Truncating the hash to 32 bits (the
    // former key) collided two names in one file at birthday odds, and the
    // loser silently resolved to the winner's symbol.
    // Foreign-receiver calls resolve under extra gates that depend on the
    // SOURCE symbol (never-self, no-guess), so they get their own cache slot
    // keyed by the source as well — sharing the bare-name entry would leak
    // one caller's exclusion to every other call site of the name.
    uint64_t name_hash = fnv1a_hash_name(full_name);
    bool rust_owner = false;
    if (auto mit = file_resolution_meta_.find(owner_fid);
        mit != file_resolution_meta_.end() &&
        mit->second.language_family == LangFamily::kRust) {
        rust_owner = true;
    }
    if (ref.foreign_receiver || rust_owner) {
        // Rust bare-call resolution is source-dependent (a method's bare
        // self-name call is the shadowing free function), so Rust refs get
        // per-source cache slots too.
        name_hash ^= 0x9e3779b97f4a7c15ULL *
                     (static_cast<uint64_t>(ref.source_symbol) | 1ULL);
    }
    // Type-position refs resolve against a different candidate set, so they
    // must not share cache entries with same-named value/call refs.
    if (ref.type_position) name_hash ^= 0xd6e8feb86659fd93ULL;
    // Arity-preferring resolution: a known call-site argument count changes
    // the answer among same-name overloads, so it gets its own cache slot.
    const uint8_t call_args = ref.stored_arg_count();
    if (call_args != 255) {
        name_hash ^= 0xa0761d6478bd642fULL * (static_cast<uint64_t>(call_args) + 1);
    }
    std::pair<FileID, uint64_t> cache_key{owner_fid, name_hash};

    if (auto it = reference_cache_.find(cache_key);
        it != reference_cache_.end()) {
        return it->second;
    }

    // Scope-typed method ref "Type.M" (emitted by the extractor when the
    // receiver's type is locally known): resolve to the method named M whose
    // receiver/owning type is Type — the precise target among same-named
    // methods. Bare lookup name is M; on no receiver-type match we fall through
    // to the name-based path on M (so unknown/dynamic receivers degrade to the
    // existing behavior rather than failing).
    std::string_view name = full_name;
    std::string_view recv_type;
    bool typed_receiver_miss = false;
    if (auto dot = full_name.rfind('.'); dot != std::string::npos) {
        recv_type = std::string_view(full_name).substr(0, dot);
        name = std::string_view(full_name).substr(dot + 1);
        if (!recv_type.empty() && !name.empty()) {
            // Among the type's same-named methods (overloads), a known
            // call-site argument count picks the exact-arity sibling;
            // without one — or with no exact match (default/variadic
            // parameters) — the first receiver-type match stands. Arity
            // PREFERS, never drops (okhttp: create(x) inside create(x, y)
            // collapsed into false recursion).
            SymbolID typed_first = 0;
            for (SymbolID id : s.symbols.get_symbols_by_name(name)) {
                if (const auto* sym = s.symbols.get(id)) {
                    if (!symbol_matches_receiver_type(*sym, recv_type))
                        continue;
                    if (call_args != 255 &&
                        sym->parameter_count == call_args) {
                        reference_cache_[cache_key] = id;
                        return id;
                    }
                    if (typed_first == 0) typed_first = id;
                }
            }
            if (typed_first != 0) {
                reference_cache_[cache_key] = typed_first;
                return typed_first;
            }
            // The receiver's type is KNOWN and no method of that type has
            // this name (a stdlib/extension call the index cannot see, or an
            // inherited method on a supertype). That evidence positively
            // excludes every same-named method on unrelated types, so the
            // name-only paths below (same-file fast path, unique-candidate /
            // same-dir fallback) are forbidden — they are what resolved every
            // Kotlin stdlib `.apply {}` to ConnectionSpec.apply (reach 1 ->
            // 155, okhttp audit) and zls initCapacity to reach=146. Import
            // evidence still resolves; a missing edge is the cheaper error.
            typed_receiver_miss = true;
        }
    }

    bool exclude_source = false;
    // Check same-file symbols first (fast path). A foreign-receiver call of
    // unknown type takes NO name-only match at all — not the calling symbol
    // (the `files.size()` false self-loop class) and not its same-file
    // neighbors either: every `.as_ref()` inside glob.rs resolving to
    // `Glob::as_ref` gave a 3-line trait impl reach=152 (ripgrep audit).
    // Such calls resolve only through receiver-type or import evidence.
    if (!ref.foreign_receiver && !typed_receiver_miss) {
        // Rust bare calls inside a METHOD can never be the method itself
        // (method calls require `self.`): `trim_line_terminator(...)` inside
        // StandardImpl::trim_line_terminator is the same-named FREE function,
        // and picking the method reported fake recursion (ripgrep audit).
        SymbolID first_match = 0, other_match = 0, exact_arity = 0;
        for (SymbolID id : file_symbol_ids) {
            if (const auto* sym = s.symbols.get(id)) {
                if (sym->symbol.name != name) continue;
                if (ref.type_position &&
                    !is_type_like_symbol(sym->symbol.type))
                    continue;
                if (first_match == 0) first_match = id;
                if (id != ref.source_symbol && other_match == 0)
                    other_match = id;
                if (call_args != 255 && exact_arity == 0 &&
                    sym->parameter_count == call_args) {
                    exact_arity = id;
                }
            }
        }
        // A known argument count picks the exact-arity overload; a bare call
        // matching a same-named sibling's arity but not the enclosing
        // function's is delegation, not recursion (okhttp audit: 6/8
        // recursion entries were this). No exact match keeps first_match —
        // default/variadic parameters make a smaller call legal.
        if (exact_arity != 0) first_match = exact_arity;
        bool rust_method_self_shadow = false;
        if (rust_owner && first_match == ref.source_symbol) {
            const auto* src = s.symbols.get(ref.source_symbol);
            if (src != nullptr && src->symbol.type == SymbolType::Method) {
                if (other_match != 0) {
                    reference_cache_[cache_key] = other_match;
                    return other_match;
                }
                // Only the method itself matches in this file; the real
                // callee (a same-named free function, often in another
                // crate) must come from cross-file resolution with the
                // method excluded.
                rust_method_self_shadow = true;
                first_match = 0;
            }
        }
        if (first_match != 0) {
            reference_cache_[cache_key] = first_match;
            return first_match;
        }
        if (rust_method_self_shadow) exclude_source = true;
    }

    // Cross-file: gate candidates by language family first — a call in a
    // Python file must not resolve to a same-named symbol in a vendored C++
    // tree just because the name matches (unknown families stay eligible).
    auto candidates = s.symbols.get_symbols_by_name(name);
    SymbolID resolved = 0;
    if (!candidates.empty()) {
        LangFamily ref_family = LangFamily::kUnknown;
        bool ref_low_quality = false;
        uint64_t ref_dir_hash = 0;
        if (auto mit = file_resolution_meta_.find(owner_fid);
            mit != file_resolution_meta_.end()) {
            ref_family = mit->second.language_family;
            ref_low_quality = mit->second.low_quality;
            ref_dir_hash = mit->second.dir_hash;
        }
        // Inline capacity covers the overwhelmingly common 1-2 candidate
        // case; this runs per cache-miss on the index-build path, so a
        // throwaway heap allocation here is against the perf mandate.
        //
        // Eligibility gates (D2 — a wrong edge inflates reach for an
        // unrelated symbol, so ineligible candidates are dropped up front):
        //   - language family: a Python call never links into a vendored
        //     C++ tree (unknown families stay eligible);
        //   - quality: a PRODUCTION caller never links into test/example/
        //     vendored/generated code (guzzle's tests/bootstrap.php
        //     curl_setopt shim collected every production call). Low-quality
        //     callers keep all candidates — tests legitimately call helpers.
        absl::InlinedVector<SymbolID, 8> filtered;
        SymbolID same_dir = 0;
        int same_dir_count = 0;
        for (SymbolID id : candidates) {
            if ((ref.foreign_receiver || exclude_source) &&
                id == ref.source_symbol)
                continue;
            const auto* sym = s.symbols.get(id);
            if (sym == nullptr) continue;
            // Type positions never resolve to a function/variable: the
            // class-vs-constructor name tie is what turned every C++ type
            // use into an ambiguous no-edge.
            if (ref.type_position && !is_type_like_symbol(sym->symbol.type))
                continue;
            LangFamily family = LangFamily::kUnknown;
            bool low_quality = false;
            uint64_t dir_hash = 0;
            if (auto it2 = file_resolution_meta_.find(sym->symbol.file_id);
                it2 != file_resolution_meta_.end()) {
                family = it2->second.language_family;
                low_quality = it2->second.low_quality;
                dir_hash = it2->second.dir_hash;
            }
            if (ref_family != LangFamily::kUnknown &&
                family != LangFamily::kUnknown && family != ref_family) {
                continue;
            }
            if (!ref_low_quality && low_quality) continue;
            filtered.push_back(id);
            if (dir_hash == ref_dir_hash) {
                same_dir = id;
                ++same_dir_count;
            }
        }

        resolved = import_resolver_.resolve_symbol_reference(
            owner_fid, name, filtered,
            [&s](SymbolID id) { return s.symbols.get(id); },
            ref.foreign_receiver || typed_receiver_miss);

        // Ambiguous-name fallback: no decisive import/same-file/export
        // evidence. Package proximity breaks the tie — a unique candidate in
        // the caller's own directory is the target (Go package = directory).
        // Failing that, a unique eligible candidate links. Anything still
        // ambiguous builds NO edge: reach/depended_on_by computed over
        // guessed edges is noise, and a missing edge is the cheaper error.
        // Foreign-receiver calls of unknown type never take this guess at
        // all — `x.Add()` linking to the one `Add` the corpus happens to
        // index is exactly the edge class that inflated LOAD BEARING reach.
        if (resolved == 0 && !ref.foreign_receiver &&
            !typed_receiver_miss) {
            if (same_dir_count == 1) {
                resolved = same_dir;
            } else if (filtered.size() == 1) {
                resolved = filtered[0];
            }
        }
    }

    reference_cache_[cache_key] = resolved;
    return resolved;
}

void ReferenceTracker::update_reference_stats(Snapshot& s) {
    auto ids = s.symbols.get_ids();
    for (SymbolID id : ids) {
        update_reference_stats_for_symbol(s, id);
    }

    // Update global stats.
    absl::flat_hash_map<FileID, bool> files_seen;
    int sym_refs = 0;
    for (const auto& [sym_id, refs] : s.incoming_refs) {
        sym_refs += static_cast<int>(refs.size());
    }
    for (const auto& [sym_id, refs] : s.outgoing_refs) {
        sym_refs += static_cast<int>(refs.size());
    }
    s.for_each_live_ref([&](FileID fid, uint32_t, const StoredRef&) {
        files_seen[fid] = true;
    });

    s.stats.total_references = static_cast<int>(s.live_ref_count());
    s.stats.total_symbols = s.symbols.size();
    s.stats.files_with_refs = static_cast<int>(files_seen.size());
    s.stats.symbol_refs = sym_refs;
}

void ReferenceTracker::update_reference_stats_for_symbol(Snapshot& s,
                                                         SymbolID symbol_id) {
    auto* sym = s.symbols.get_mutable(symbol_id);
    if (sym == nullptr) return;

    std::span<const uint64_t> incoming_ids;
    std::span<const uint64_t> outgoing_ids;

    if (auto it = s.incoming_refs.find(symbol_id);
        it != s.incoming_refs.end()) {
        incoming_ids = it->second;
    }
    if (auto it = s.outgoing_refs.find(symbol_id);
        it != s.outgoing_refs.end()) {
        outgoing_ids = it->second;
    }

    // Counts only — the Reference objects stay in s.references; readers
    // that need them fetch via get_symbol_references (ID-based, on demand).
    sym->incoming_ref_count = static_cast<int>(incoming_ids.size());
    sym->outgoing_ref_count = static_cast<int>(outgoing_ids.size());

}

std::vector<SymbolID> ReferenceTracker::get_symbols_by_ref_type(
    SymbolID symbol_id, bool incoming, ReferenceType ref_type) const {

    auto snap = load_snapshot();
    const absl::flat_hash_map<SymbolID, std::vector<uint64_t>>& ref_map =
        incoming ? snap->incoming_refs : snap->outgoing_refs;

    auto it = ref_map.find(symbol_id);
    if (it == ref_map.end()) return {};

    absl::flat_hash_map<SymbolID, bool> seen;
    std::vector<SymbolID> result;

    for (uint64_t ref_id : it->second) {
        const StoredRef* ref_ptr = snap->find_ref(ref_id);
        if (ref_ptr == nullptr) continue;
        const auto& ref = *ref_ptr;
        if (ref.type != ref_type) continue;

        SymbolID target = incoming ? ref.source_symbol : ref.target_symbol;
        if (target == 0 || seen.contains(target)) continue;
        seen[target] = true;
        result.push_back(target);
    }

    return result;
}

FunctionTreeNode ReferenceTracker::build_tree_node(
    const Snapshot& s, SymbolID symbol_id, int depth, int max_depth,
    absl::flat_hash_map<SymbolID, bool>& visited) const {

    FunctionTreeNode node;
    if (depth > max_depth || visited.contains(symbol_id)) return node;
    visited[symbol_id] = true;

    const auto* sym = s.symbols.get(symbol_id);
    if (sym == nullptr) return node;

    node.name = sym->symbol.name;
    node.symbol_id = symbol_id;
    node.file_id = sym->symbol.file_id;
    node.line = sym->symbol.line;

    auto refs = s.get_symbol_references(symbol_id, "outgoing");
    for (const auto& ref : refs) {
        if (ref.type == ReferenceType::Call && ref.target_symbol != 0) {
            auto child = build_tree_node(s, ref.target_symbol, depth + 1,
                                          max_depth, visited);
            if (!child.name.empty()) {
                node.children.push_back(std::move(child));
            }
        }
    }

    return node;
}

}  // namespace lci
