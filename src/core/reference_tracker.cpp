#include <lci/core/reference_tracker.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <string>

namespace lci {

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

std::vector<Reference> ReferenceTracker::Snapshot::get_references_by_id(
    std::span<const uint64_t> ref_ids) const {
    std::vector<Reference> result;
    result.reserve(ref_ids.size());
    for (uint64_t id : ref_ids) {
        if (auto it = references.find(id); it != references.end()) {
            result.push_back(it->second);
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

ReferenceTracker::Snapshot::LineMapHandle
ReferenceTracker::Snapshot::get_file_line_to_symbols(FileID file_id) const {
    auto it = line_to_symbols_by_file.find(file_id);
    if (it == line_to_symbols_by_file.end()) return nullptr;
    return {shared_from_this(), &it->second};
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
    scope_chain_cache_.clear();
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

    write_snapshot([&](Snapshot& s) {
        s.scopes_by_file[file_id].assign(scopes.begin(), scopes.end());

        for (const auto& sym : symbols) {
            SymbolID id = next_symbol_id_++;
            auto scope_chain = build_symbol_scope_chain(sym, scopes);

            Symbol sm = sym;
            sm.file_id = file_id;

            bool is_exported = compute_is_exported(path, sm.name);

            EnhancedSymbol es;
            es.symbol = std::move(sm);
            es.id = id;
            es.scope_chain = scope_chain;
            es.is_exported = is_exported;

            s.symbols.set(id, es);
            s.symbol_scopes[id] = std::move(scope_chain);

            enhanced.push_back(std::move(es));
        }

        // Store references for later processing.
        for (const auto& ref : references) {
            Reference r = ref;
            r.file_id = file_id;
            uint64_t global_id =
                make_global_ref_id(file_id, static_cast<uint32_t>(r.id));
            r.id = global_id;
            s.references[r.id] = std::move(r);
        }
    });

    return enhanced;
}

void ReferenceTracker::apply_enrichment(
    std::span<const EnhancedSymbol> enriched) {
    if (enriched.empty()) return;
    write_snapshot([&](Snapshot& s) {
        for (const auto& es : enriched) {
            s.symbols.set(es.id, es);
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

        for (auto& [ref_id, ref] : s.references) {
            SymbolID source_id = ref.source_symbol;
            SymbolID target_id = ref.target_symbol;

            if (source_id == 0) {
                source_id = find_symbol_at_location(s, ref.file_id, ref.line,
                                                     ref.column);
                if (source_id != 0) ref.source_symbol = source_id;
            }
            if (target_id == 0) {
                auto it = symbols_by_file.find(ref.file_id);
                std::span<const SymbolID> file_syms;
                if (it != symbols_by_file.end()) {
                    file_syms = it->second;
                }
                target_id = resolve_reference_target(s, ref, file_syms);
                if (target_id != 0) ref.target_symbol = target_id;
            }

            if (source_id != 0) {
                s.outgoing_refs[source_id].push_back(ref_id);
            }
            if (target_id != 0) {
                s.incoming_refs[target_id].push_back(ref_id);
            }
        }

        update_reference_stats(s);
    });
}

void ReferenceTracker::remove_file(FileID file_id) {
    write_snapshot([&](Snapshot& s) {
        auto file_syms = s.symbols.get_symbols_by_file(file_id);
        std::vector<SymbolID> ids(file_syms.begin(), file_syms.end());

        for (SymbolID sym_id : ids) {
            // Remove outgoing references.
            if (auto it = s.outgoing_refs.find(sym_id);
                it != s.outgoing_refs.end()) {
                for (uint64_t ref_id : it->second) {
                    auto ref_it = s.references.find(ref_id);
                    if (ref_it != s.references.end()) {
                        if (ref_it->second.target_symbol != 0) {
                            remove_from_incoming_refs(
                                s, ref_it->second.target_symbol, ref_id);
                        }
                        s.references.erase(ref_it);
                    }
                }
                s.outgoing_refs.erase(it);
            }

            // Remove incoming references.
            if (auto it = s.incoming_refs.find(sym_id);
                it != s.incoming_refs.end()) {
                for (uint64_t ref_id : it->second) {
                    auto ref_it = s.references.find(ref_id);
                    if (ref_it != s.references.end()) {
                        if (ref_it->second.source_symbol != 0) {
                            remove_from_outgoing_refs(
                                s, ref_it->second.source_symbol, ref_id);
                        }
                    }
                }
                s.incoming_refs.erase(it);
            }

            s.symbols.remove(sym_id);
            s.symbol_scopes.erase(sym_id);
        }

        s.scopes_by_file.erase(file_id);
        s.line_to_symbols_by_file.erase(file_id);
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
    out.reserve(snap->references.size());
    for (const auto& [id, ref] : snap->references) {
        out.push_back(ref);
    }
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

// -- Line-to-symbol index ----------------------------------------------------

void ReferenceTracker::store_line_to_symbols(
    FileID file_id,
    absl::flat_hash_map<int, std::vector<int>> line_to_symbols) {
    if (line_to_symbols.empty()) return;
    write_snapshot([&](Snapshot& s) {
        s.line_to_symbols_by_file[file_id] = std::move(line_to_symbols);
    });
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

    auto ext_pos = path.rfind('.');
    if (ext_pos == std::string_view::npos) return true;
    auto ext = path.substr(ext_pos);

    if (ext == ".go") {
        return std::isupper(static_cast<unsigned char>(symbol_name[0])) != 0;
    }
    if (ext == ".py") {
        return !symbol_name.starts_with('_');
    }
    if (ext == ".js" || ext == ".jsx" || ext == ".ts" || ext == ".tsx" ||
        ext == ".mjs" || ext == ".cjs") {
        return !symbol_name.starts_with('_') && !symbol_name.starts_with('#');
    }
    if (ext == ".rb") {
        return !symbol_name.starts_with('_');
    }
    // C/C++, Java, Kotlin, Rust, and unknown: assume exported.
    return true;
}

std::vector<ScopeInfo> ReferenceTracker::build_symbol_scope_chain(
    const Symbol& symbol, std::span<const ScopeInfo> scopes) {

    int scope_count = 0;
    uint64_t cache_key = create_scope_chain_cache_key(symbol, scopes,
                                                       scope_count);

    if (auto it = scope_chain_cache_.find(cache_key);
        it != scope_chain_cache_.end()) {
        const auto& entry = it->second;
        if (entry.symbol_line == symbol.line &&
            entry.symbol_end_line == symbol.end_line &&
            entry.scope_count == scope_count) {
            return entry.scope_chain;
        }
    }

    std::vector<ScopeInfo> chain;
    chain.reserve(4);
    for (const auto& scope : scopes) {
        if (scope.start_line <= symbol.line &&
            (scope.end_line == 0 || scope.end_line >= symbol.line)) {
            chain.push_back(scope);
        }
    }

    scope_chain_cache_[cache_key] = ScopeChainCacheEntry{
        .scope_chain = chain,
        .symbol_line = symbol.line,
        .symbol_end_line = symbol.end_line,
        .scope_count = scope_count,
    };

    return chain;
}

uint64_t ReferenceTracker::create_scope_chain_cache_key(
    const Symbol& symbol, std::span<const ScopeInfo> scopes,
    int& scope_count_out) const {

    uint64_t h = kFnvOffset64;
    h ^= static_cast<uint64_t>(symbol.line);
    h *= kFnvPrime64;
    h ^= static_cast<uint64_t>(symbol.end_line);
    h *= kFnvPrime64;

    int count = 0;
    for (const auto& scope : scopes) {
        if (count >= 3) {
            if (scope.start_line <= symbol.line &&
                (scope.end_line == 0 || scope.end_line >= symbol.line)) {
                h ^= static_cast<uint64_t>(scope.start_line);
                h *= kFnvPrime64;
                h ^= static_cast<uint64_t>(scope.end_line);
                h *= kFnvPrime64;
                count++;
                break;
            }
        } else {
            h ^= static_cast<uint64_t>(scope.start_line);
            h *= kFnvPrime64;
            h ^= static_cast<uint64_t>(scope.end_line);
            h *= kFnvPrime64;
            count++;
        }
    }

    scope_count_out = count;
    return h;
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
}  // namespace

SymbolID ReferenceTracker::resolve_reference_target(
    const Snapshot& s, const Reference& ref,
    std::span<const SymbolID> file_symbol_ids) {

    const auto& full_name = ref.referenced_name;
    if (full_name.empty()) return 0;

    uint64_t name_hash = fnv1a_hash_name(full_name);
    uint64_t cache_key = (static_cast<uint64_t>(ref.file_id) << 32) |
                          (name_hash & 0xFFFFFFFF);

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
    if (auto dot = full_name.rfind('.'); dot != std::string::npos) {
        recv_type = std::string_view(full_name).substr(0, dot);
        name = std::string_view(full_name).substr(dot + 1);
        if (!recv_type.empty() && !name.empty()) {
            for (SymbolID id : s.symbols.get_symbols_by_name(name)) {
                if (const auto* sym = s.symbols.get(id)) {
                    if (symbol_matches_receiver_type(*sym, recv_type)) {
                        reference_cache_[cache_key] = id;
                        return id;
                    }
                }
            }
        }
    }

    // Check same-file symbols first (fast path).
    for (SymbolID id : file_symbol_ids) {
        if (const auto* sym = s.symbols.get(id)) {
            if (sym->symbol.name == name) {
                reference_cache_[cache_key] = id;
                return id;
            }
        }
    }

    // Cross-file: use import resolver.
    auto candidates = s.symbols.get_symbols_by_name(name);
    SymbolID resolved = 0;
    if (!candidates.empty()) {
        resolved = import_resolver_.resolve_symbol_reference(
            ref.file_id, name, candidates,
            [&s](SymbolID id) { return s.symbols.get(id); });
        if (resolved == 0) resolved = candidates[0];
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
    for (const auto& [id, ref] : s.references) {
        files_seen[ref.file_id] = true;
    }

    s.stats.total_references = static_cast<int>(s.references.size());
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

    // Populate incoming/outgoing ref vectors on the symbol.
    sym->incoming_refs = s.get_references_by_id(incoming_ids);
    sym->outgoing_refs = s.get_references_by_id(outgoing_ids);

    // Build stats on the total RefCount.
    absl::flat_hash_map<FileID, bool> in_files;
    absl::flat_hash_map<FileID, bool> out_files;
    RefStrengthStats in_str{};
    RefStrengthStats out_str{};
    int usage_incoming = 0;

    for (uint64_t rid : incoming_ids) {
        if (auto it = s.references.find(rid); it != s.references.end()) {
            const auto& r = it->second;
            in_files[r.file_id] = true;
            if (r.type != ReferenceType::Import) usage_incoming++;
            switch (r.strength) {
                case RefStrength::Tight: in_str.tight++; break;
                case RefStrength::Loose: in_str.loose++; break;
                case RefStrength::Transitive: in_str.transitive++; break;
            }
        }
    }
    for (uint64_t rid : outgoing_ids) {
        if (auto it = s.references.find(rid); it != s.references.end()) {
            const auto& r = it->second;
            out_files[r.file_id] = true;
            switch (r.strength) {
                case RefStrength::Tight: out_str.tight++; break;
                case RefStrength::Loose: out_str.loose++; break;
                case RefStrength::Transitive: out_str.transitive++; break;
            }
        }
    }

    std::vector<FileID> in_file_list;
    in_file_list.reserve(in_files.size());
    for (const auto& [fid, _] : in_files) in_file_list.push_back(fid);

    std::vector<FileID> out_file_list;
    out_file_list.reserve(out_files.size());
    for (const auto& [fid, _] : out_files) out_file_list.push_back(fid);

    RefCount total{};
    total.incoming_count = usage_incoming;
    total.outgoing_count = static_cast<int>(outgoing_ids.size());
    total.incoming_files = std::move(in_file_list);
    total.outgoing_files = std::move(out_file_list);
    total.strength.tight = in_str.tight + out_str.tight;
    total.strength.loose = in_str.loose + out_str.loose;
    total.strength.transitive = in_str.transitive + out_str.transitive;

    // All scope levels get the same aggregate for now.
    RefStats rs{};
    rs.folder_level = total;
    rs.file_level = total;
    rs.class_level = total;
    rs.function_level = total;
    rs.variable_level = total;
    rs.total = total;

    // RefStats is not stored on EnhancedSymbol in C++ currently;
    // the incoming_refs/outgoing_refs vectors serve the same purpose.
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
        auto ref_it = snap->references.find(ref_id);
        if (ref_it == snap->references.end()) continue;
        const auto& ref = ref_it->second;
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
