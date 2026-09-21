#include <lci/mcp/context_manifest_expander.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <lci/core/file_content_store.h>
#include <lci/core/reference_tracker.h>
#include <lci/indexing/master_index.h>

namespace lci {
namespace mcp {

// -- parse_expansion_directive ------------------------------------------------

std::pair<std::string, int> parse_expansion_directive(
    std::string_view directive) {
    auto colon = directive.find(':');
    if (colon == std::string_view::npos) {
        return {std::string(directive), 1};
    }
    auto type = std::string(directive.substr(0, colon));
    int depth = 1;
    auto depth_str = directive.substr(colon + 1);
    try {
        depth = std::stoi(std::string(depth_str));
        if (depth < 1) depth = 1;
    } catch (...) {
        // Invalid depth, default to 1
    }
    return {type, depth};
}

// -- ExpansionEngine ----------------------------------------------------------

ExpansionEngine::ExpansionEngine(MasterIndex& index) : index_(index) {}

// -- Path resolution ----------------------------------------------------------

std::string ExpansionEngine::resolve_path(const std::string& file,
                                           const std::string& project_root) {
    // Keys in MasterIndex::path_to_id are forward-slash on every platform
    // (the scanner stores generic_string()), and lookups are exact string
    // matches. Build the lookup key the same way: normalize to forward-slash
    // and use std::filesystem to join, so a native-separator project_root
    // (e.g. "C:\\proj" on Windows) still yields a '/'-joined key. On POSIX
    // this is byte-identical to the old "root + '/' + file" concatenation.
    if (file.empty()) return {};
    std::filesystem::path p(file);
    if (p.is_absolute()) return p.generic_string();
    if (project_root.empty()) return p.generic_string();
    return (std::filesystem::path(project_root) / p).generic_string();
}

std::string ExpansionEngine::get_file_path(FileID file_id) {
    return index_.get_file_path(file_id);
}

// -- Source extraction --------------------------------------------------------

ExpansionEngine::LinesResult ExpansionEngine::extract_source_by_lines(
    const std::string& file_path, int start_line, int end_line) {
    auto fid = index_.path_to_id(file_path);
    if (fid == 0) {
        return {{}, "file not found: " + file_path};
    }

    auto& store = index_.file_content_store();
    // Pin ONE FileContent for the whole extraction. Reading content and
    // line_offsets via separate store calls would anchor them to different
    // snapshot loads through the single-slot thread-local read_pin: a
    // concurrent reindex between the two calls would dangle the first view and
    // mismatch its length against the second's offsets (OOB / UAF).
    auto fc = store.get_file(fid);
    if (!fc) {
        return {{}, "file content not found: " + file_path};
    }
    std::string_view content = fc->view();
    if (content.empty()) {
        return {{}, "file content not found: " + file_path};
    }

    const std::vector<uint32_t>& offsets = fc->line_offsets;
    if (offsets.empty()) {
        return {{}, "empty file: " + file_path};
    }

    int line_count = static_cast<int>(offsets.size());

    if (start_line < 1 || start_line > line_count) {
        return {{}, "start line " + std::to_string(start_line) +
                    " out of range (file has " + std::to_string(line_count) +
                    " lines)"};
    }
    if (end_line < start_line) end_line = start_line;
    if (end_line > line_count) end_line = line_count;

    // offsets are 0-based byte offsets; lines are 1-indexed
    auto begin_off = offsets[static_cast<size_t>(start_line - 1)];
    size_t end_off;
    if (end_line < line_count) {
        end_off = offsets[static_cast<size_t>(end_line)];
    } else {
        end_off = content.size();
    }

    // Trim trailing newline
    while (end_off > begin_off && (content[end_off - 1] == '\n' ||
                                    content[end_off - 1] == '\r')) {
        --end_off;
    }

    return {std::string(content.substr(begin_off, end_off - begin_off)), ""};
}

// -- Identity resolution ------------------------------------------------------

ExpansionEngine::ResolveStart ExpansionEngine::resolve_start(
    const std::string& file_path, const std::string& symbol_name) {
    auto& tracker = index_.ref_tracker();
    auto snap = tracker.pin();
    ResolveStart out;

    const bool file_scoped = !file_path.empty();
    FileID fid = 0;
    if (file_scoped) {
        fid = index_.path_to_id(file_path);
        if (fid == 0) {
            out.status = RefResolution::MissingFile;
            return out;
        }
    }

    auto handles = snap->find_symbols_by_name(symbol_name);
    std::vector<ReferenceTracker::Snapshot::SymbolHandle> matches;
    for (const auto& h : handles) {
        if (!h) continue;
        if (file_scoped && h->symbol.file_id != fid) continue;
        matches.push_back(h);
    }

    if (matches.empty()) {
        // A file-scoped miss is a hard stop: never fall back to a same-name
        // symbol from another file (that would hydrate unrelated code under
        // the requested file's identity).
        out.status = RefResolution::MissingSymbol;
        return out;
    }
    if (matches.size() > 1 && file_scoped) {
        out.status = RefResolution::AmbiguousSymbol;
        return out;
    }

    // Symbol-only refs (no file) keep legacy first-match behavior, made
    // deterministic by (file_id, line) so the same index always yields the
    // same symbol.
    if (!file_scoped && matches.size() > 1) {
        std::sort(matches.begin(), matches.end(),
                  [](const auto& a, const auto& b) {
                      if (a->symbol.file_id != b->symbol.file_id) {
                          return a->symbol.file_id < b->symbol.file_id;
                      }
                      return a->symbol.line < b->symbol.line;
                  });
    }
    out.id = matches.front()->id;
    out.status = RefResolution::Resolved;
    return out;
}

ExpansionEngine::ExtractResult ExpansionEngine::extract_symbol_source(
    const std::string& file_path, const std::string& symbol_name,
    FormatType /*format*/) {
    // Saved line hints are deliberately ignored for symbol refs: a hint is a
    // position from an earlier index and, after edits, points at unrelated
    // code. The symbol's identity (resolved inside its own file) governs.
    auto rs = resolve_start(file_path, symbol_name);
    if (rs.status != RefResolution::Resolved) {
        std::string err;
        switch (rs.status) {
            case RefResolution::MissingFile:
                err = "file not found in index: " + file_path;
                break;
            case RefResolution::MissingSymbol:
                err = "symbol " + symbol_name +
                      " not found" +
                      (file_path.empty() ? "" : " in file " + file_path);
                break;
            case RefResolution::AmbiguousSymbol:
                err = "symbol " + symbol_name +
                      " is ambiguous in file " + file_path +
                      " (multiple same-name definitions)";
                break;
            default:
                err = "unresolved symbol " + symbol_name;
        }
        return {{}, {}, {}, std::move(err), rs.status};
    }

    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto sym = rt_snap->get_enhanced_symbol(rs.id);
    if (!sym) {
        return {{}, {}, {}, "symbol " + symbol_name + " not found",
                RefResolution::MissingSymbol};
    }

    auto sym_path = get_file_path(sym->symbol.file_id);
    int start = sym->symbol.line;
    int end = sym->symbol.end_line;
    if (end < start) end = start;

    auto [source, err] = extract_source_by_lines(
        sym_path.empty() ? file_path : sym_path, start, end);
    if (!err.empty()) {
        return {{}, {}, {}, err, RefResolution::Resolved};
    }

    SymbolInfo info;
    info.start_line = start;
    info.end_line = end;
    info.symbol_type = std::string(to_string(sym->symbol.type));
    info.is_exported = sym->is_exported;
    info.signature = sym->signature;
    if (info.signature.empty()) {
        auto nl = source.find('\n');
        auto line = (nl != std::string::npos) ? source.substr(0, nl) : source;
        auto first = line.find_first_not_of(" \t");
        info.signature =
            (first != std::string::npos) ? line.substr(first) : line;
    }

    return {std::move(source), {start, end}, std::move(info), {},
            RefResolution::Resolved};
}

namespace {
// Finding 3: format="outline" lists the file's own symbols (name + line)
// rather than any single symbol's body — a directory-listing-shaped view,
// not a source excerpt.
std::string build_file_outline(MasterIndex& index, const std::string& file_path) {
    auto fid = index.path_to_id(file_path);
    if (fid == 0) return "";
    auto& tracker = index.ref_tracker();
    auto rt_snap = tracker.pin();
    auto symbols = rt_snap->get_file_enhanced_symbols(fid);
    std::vector<const EnhancedSymbol*> sorted;
    sorted.reserve(symbols.size());
    for (const auto& es : symbols) {
        if (es) sorted.push_back(es.get());
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) {
        return a->symbol.line < b->symbol.line;
    });
    std::string out;
    for (const auto* es : sorted) {
        if (!out.empty()) out += '\n';
        out += std::to_string(es->symbol.line);
        out += ": ";
        out += to_string(es->symbol.type);
        out += ' ';
        out += es->symbol.name;
    }
    return out;
}
}  // namespace

// -- hydrate_reference --------------------------------------------------------

ExpansionEngine::HydrateResult ExpansionEngine::hydrate_reference(
    const ContextRef& ref, FormatType format,
    const std::string& project_root) {
    HydratedRef hr;
    hr.file = ref.file;
    hr.symbol = ref.symbol;
    hr.role = ref.role;
    hr.note = ref.note;

    auto file_path = resolve_path(ref.file, project_root);

    // Resolve a file path's index status without ever reading the file body:
    // a file in the index hydrates a symbol listing (possibly empty for a
    // no-symbol file such as a contract JSON); a file on disk but not in the
    // index is FileNotIndexed (honest, never a silent full-file read); a file
    // neither in the index nor on disk is MissingFile.
    auto file_presence = [&](const std::string& key) -> RefResolution {
        if (index_.path_to_id(key) != 0) return RefResolution::Resolved;
        std::error_code ec;
        if (std::filesystem::exists(key, ec)) {
            return RefResolution::FileNotIndexed;
        }
        return RefResolution::MissingFile;
    };

    // A file-only ref (file, no symbol, no line range) is a request for the
    // whole file as context. Rather than reject it or read the full body,
    // return the file's own symbol outline: name + kind + current line for
    // each indexed symbol, empty-but-resolved for a no-symbol file. This lets
    // one manifest mix targeted symbol source (below) with unselected files.
    if (ref.symbol.empty() && !ref.has_line_range) {
        auto status = file_presence(file_path);
        if (status != RefResolution::Resolved) {
            std::string err = status == RefResolution::FileNotIndexed
                                  ? "file is not indexed: " + ref.file
                                  : "file not found: " + ref.file;
            return {{}, 0, err, status};
        }
        hr.source = build_file_outline(index_, file_path);  // "" is valid here
        int tokens = static_cast<int>(hr.source.size()) / 4;
        return {std::move(hr), tokens, {}, RefResolution::Resolved};
    }

    // An explicit global outline: same listing, but a saved file+symbol still
    // has to resolve by identity inside that file first — a missing file, an
    // absent symbol, or a same-file overload set is reported unresolved, never
    // masked by a full-file listing labelled with that symbol. A pure line-
    // range ref (no symbol) keeps literal current-index line semantics and is
    // not widened into a whole-file outline, so it falls through to Case 2.
    if (format == FormatType::Outline && !ref.symbol.empty()) {
        auto rs = resolve_start(file_path, ref.symbol);
        if (rs.status != RefResolution::Resolved) {
            return {{}, 0, "outline ref unresolved in " + ref.file, rs.status};
        }
        hr.source = build_file_outline(index_, file_path);
        if (hr.source.empty()) {
            return {{}, 0, "no symbols found for outline: " + ref.file,
                    RefResolution::Resolved};
        }
        int tokens = static_cast<int>(hr.source.size()) / 4;
        return {std::move(hr), tokens, {}, RefResolution::Resolved};
    }

    if (!ref.symbol.empty()) {
        // Case 1: Symbol name provided. Resolved by identity inside the
        // named file; any saved line hint is ignored.
        auto [source, lines, info, err, reason] =
            extract_symbol_source(file_path, ref.symbol, format);
        if (!err.empty() || reason != RefResolution::Resolved) {
            return {{}, 0, err, reason};
        }
        hr.source = std::move(source);
        hr.lines = lines;
        hr.symbol_type = info.symbol_type;
        hr.signature = info.signature;
        hr.is_exported = info.is_exported;
        if (format == FormatType::Signatures) extract_signature_only(hr);
    } else if (ref.has_line_range) {
        // Case 2: Only line range. Literal current-index lines, with no
        // semantic stability across edits — flagged so consumers know.
        if (!ref.file.empty() && index_.path_to_id(file_path) == 0) {
            return {{}, 0, "file not found: " + ref.file,
                    RefResolution::MissingFile};
        }
        auto [source, err] = extract_source_by_lines(
            file_path, ref.line_range.start, ref.line_range.end);
        if (!err.empty()) {
            return {{}, 0, err, RefResolution::InvalidRef};
        }
        hr.source = std::move(source);
        hr.lines = ref.line_range;
        hr.is_line_range_literal = true;
    } else {
        return {{}, 0, "reference must have either symbol name or line range",
                RefResolution::InvalidRef};
    }

    int tokens = static_cast<int>(hr.source.size()) / 4;
    return {std::move(hr), tokens, {}, RefResolution::Resolved};
}

ExpansionEngine::HydrateResult ExpansionEngine::hydrate_symbol_id(
    SymbolID id, FormatType format) {
    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto sym = rt_snap->get_enhanced_symbol(id);
    if (!sym) {
        return {{}, 0, "expansion target symbol id not found",
                RefResolution::MissingSymbol};
    }
    auto file_path = get_file_path(sym->symbol.file_id);
    if (file_path.empty()) {
        return {{}, 0, "expansion target file not found",
                RefResolution::MissingFile};
    }

    HydratedRef hr;
    hr.file = file_path;
    hr.symbol = sym->symbol.name;
    int start = sym->symbol.line;
    int end = sym->symbol.end_line;
    if (end < start) end = start;
    hr.lines = {start, end};
    hr.symbol_type = std::string(to_string(sym->symbol.type));
    hr.is_exported = sym->is_exported;
    hr.signature = sym->signature;

    auto [source, err] = extract_source_by_lines(file_path, start, end);
    if (!err.empty()) {
        return {std::move(hr), 0, err, RefResolution::Resolved};
    }
    hr.source = std::move(source);
    if (hr.signature.empty()) {
        auto nl = hr.source.find('\n');
        auto line = (nl != std::string::npos) ? hr.source.substr(0, nl)
                                              : hr.source;
        auto first = line.find_first_not_of(" \t");
        hr.signature = (first != std::string::npos) ? line.substr(first) : line;
    }
    if (format == FormatType::Signatures) extract_signature_only(hr);

    int tokens = static_cast<int>(hr.source.size()) / 4;
    return {std::move(hr), tokens, {}, RefResolution::Resolved};
}

// -- apply_expansions ---------------------------------------------------------

ExpansionEngine::ExpansionResult ExpansionEngine::apply_expansions(
    const ContextRef& ref, HydratedRef& hydrated, FormatType format,
    int remaining_tokens, const std::string& project_root) {
    if (ref.expansions.empty()) {
        return {};
    }

    ExpansionResult out;
    int total_tokens = 0;

    for (const auto& directive : ref.expansions) {
        if (total_tokens >= remaining_tokens) break;

        auto [dtype, depth] = parse_expansion_directive(directive);
        std::vector<HydratedRef> expanded;

        int budget = remaining_tokens - total_tokens;

        if (dtype == "callers") {
            expanded = expand_callers(ref, depth, budget, project_root, format);
        } else if (dtype == "callees") {
            expanded = expand_callees(ref, depth, budget, project_root, format);
        } else if (dtype == "implementations") {
            expanded = expand_implementations(ref, budget, project_root, format);
        } else if (dtype == "interface") {
            expanded = expand_interface(ref, budget, project_root, format);
        } else if (dtype == "siblings") {
            expanded = expand_siblings(ref, budget, project_root, format);
        } else if (dtype == "tests") {
            expanded = expand_tests(ref, budget, project_root, format);
        } else if (dtype == "doc") {
            extract_documentation(hydrated);
            continue;
        } else if (dtype == "signature") {
            extract_signature_only(hydrated);
            continue;
        } else {
            continue;  // Unknown directive
        }

        for (const auto& er : expanded) {
            total_tokens += static_cast<int>(er.source.size()) / 4;
        }
        // Emit the expansion content: the caller appends these to the
        // hydrated context so the charged tokens correspond to emitted
        // source.
        out.expanded.insert(out.expanded.end(),
                            std::make_move_iterator(expanded.begin()),
                            std::make_move_iterator(expanded.end()));
    }

    out.tokens = total_tokens;
    return out;
}

// -- Expansion helpers --------------------------------------------------------

namespace {

/// Helper to hydrate a list of symbol IDs into HydratedRefs. Each target is
/// resolved by its exact SymbolID (never re-derived by name), so an expansion
/// sibling cannot be substituted by, or made ambiguous against, a same-name
/// symbol in another file.
std::vector<HydratedRef> hydrate_symbol_ids(
    ExpansionEngine& engine, const std::vector<SymbolID>& ids,
    ReferenceTracker& /*tracker*/, MasterIndex& /*index*/,
    int remaining_tokens, const std::string& /*project_root*/,
    FormatType format, absl::flat_hash_set<SymbolID>& visited) {
    std::vector<HydratedRef> results;
    int total_tokens = 0;

    for (auto id : ids) {
        if (total_tokens >= remaining_tokens) break;
        if (visited.contains(id)) continue;
        visited.insert(id);

        auto result = engine.hydrate_symbol_id(id, format);
        if (!result.error.empty()) continue;

        total_tokens += result.tokens;
        results.push_back(std::move(result.ref));
    }

    return results;
}

}  // namespace

std::vector<HydratedRef> ExpansionEngine::expand_callers(
    const ContextRef& ref, int depth, int remaining_tokens,
    const std::string& project_root, FormatType format) {
    if (ref.symbol.empty()) return {};

    auto rs = resolve_start(resolve_path(ref.file, project_root), ref.symbol);
    if (rs.status != RefResolution::Resolved) return {};

    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto sym = rt_snap->get_enhanced_symbol(rs.id);
    if (!sym) return {};

    // Level-order walk up to `depth` hops, deduped, so "callers:2" reaches
    // callers-of-callers. seen is the BFS dedup set; hydrate_symbol_ids gets
    // its own visited set seeded with only the start symbol.
    absl::flat_hash_set<SymbolID> seen;
    seen.insert(sym->id);
    std::vector<SymbolID> ordered;
    std::vector<SymbolID> frontier = tracker.get_caller_symbols(sym->id);
    for (int d = 0; d < depth && !frontier.empty(); ++d) {
        std::vector<SymbolID> next;
        for (auto id : frontier) {
            if (!seen.insert(id).second) continue;
            ordered.push_back(id);
            auto up = tracker.get_caller_symbols(id);
            next.insert(next.end(), up.begin(), up.end());
        }
        frontier = std::move(next);
    }

    absl::flat_hash_set<SymbolID> visited;
    visited.insert(sym->id);
    return hydrate_symbol_ids(*this, ordered, tracker, index_,
                              remaining_tokens, project_root, format, visited);
}

std::vector<HydratedRef> ExpansionEngine::expand_callees(
    const ContextRef& ref, int depth, int remaining_tokens,
    const std::string& project_root, FormatType format) {
    if (ref.symbol.empty()) return {};

    auto rs = resolve_start(resolve_path(ref.file, project_root), ref.symbol);
    if (rs.status != RefResolution::Resolved) return {};

    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto sym = rt_snap->get_enhanced_symbol(rs.id);
    if (!sym) return {};

    absl::flat_hash_set<SymbolID> seen;
    seen.insert(sym->id);
    std::vector<SymbolID> ordered;
    std::vector<SymbolID> frontier = tracker.get_callee_symbols(sym->id);
    for (int d = 0; d < depth && !frontier.empty(); ++d) {
        std::vector<SymbolID> next;
        for (auto id : frontier) {
            if (!seen.insert(id).second) continue;
            ordered.push_back(id);
            auto down = tracker.get_callee_symbols(id);
            next.insert(next.end(), down.begin(), down.end());
        }
        frontier = std::move(next);
    }

    absl::flat_hash_set<SymbolID> visited;
    visited.insert(sym->id);
    return hydrate_symbol_ids(*this, ordered, tracker, index_,
                              remaining_tokens, project_root, format, visited);
}

std::vector<HydratedRef> ExpansionEngine::expand_implementations(
    const ContextRef& ref, int remaining_tokens,
    const std::string& project_root, FormatType format) {
    if (ref.symbol.empty()) return {};

    // Resolve the expansion source by the same identity rule as hydration: a
    // file+symbol binds inside that file only, a same-file overload set is
    // ambiguous (return nothing rather than pick one), and a missing file or
    // symbol is never substituted from elsewhere. The targets below are then
    // hydrated by exact SymbolID, so no same-name sibling leaks in.
    auto rs = resolve_start(resolve_path(ref.file, project_root), ref.symbol);
    if (rs.status != RefResolution::Resolved) return {};

    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto target = rt_snap->get_enhanced_symbol(rs.id);
    if (!target) return {};
    SymbolID target_id = target->id;

    auto impl_ids = tracker.get_implementors(target_id);
    auto derived_ids = tracker.get_derived_types(target_id);

    // Combine
    std::vector<SymbolID> all_ids;
    all_ids.reserve(impl_ids.size() + derived_ids.size());
    all_ids.insert(all_ids.end(), impl_ids.begin(), impl_ids.end());
    all_ids.insert(all_ids.end(), derived_ids.begin(), derived_ids.end());

    absl::flat_hash_set<SymbolID> visited;
    visited.insert(target_id);

    return hydrate_symbol_ids(*this, all_ids, tracker, index_,
                              remaining_tokens, project_root, format, visited);
}

std::vector<HydratedRef> ExpansionEngine::expand_interface(
    const ContextRef& ref, int remaining_tokens,
    const std::string& project_root, FormatType format) {
    if (ref.symbol.empty()) return {};

    // Same identity rule as hydration (see expand_implementations): resolve the
    // source uniquely inside its file; never pick one of an ambiguous set or
    // substitute a same-name type from another file. Targets hydrate by exact
    // SymbolID.
    auto rs = resolve_start(resolve_path(ref.file, project_root), ref.symbol);
    if (rs.status != RefResolution::Resolved) return {};

    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto target = rt_snap->get_enhanced_symbol(rs.id);
    if (!target) return {};
    SymbolID target_id = target->id;

    auto iface_ids = tracker.get_implemented_interfaces(target_id);
    auto base_ids = tracker.get_base_types(target_id);

    std::vector<SymbolID> all_ids;
    all_ids.reserve(iface_ids.size() + base_ids.size());
    all_ids.insert(all_ids.end(), iface_ids.begin(), iface_ids.end());
    all_ids.insert(all_ids.end(), base_ids.begin(), base_ids.end());

    absl::flat_hash_set<SymbolID> visited;
    visited.insert(target_id);

    return hydrate_symbol_ids(*this, all_ids, tracker, index_,
                              remaining_tokens, project_root, format, visited);
}

std::vector<HydratedRef> ExpansionEngine::expand_siblings(
    const ContextRef& ref, int remaining_tokens,
    const std::string& project_root, FormatType format) {
    if (ref.symbol.empty()) return {};

    auto rs = resolve_start(resolve_path(ref.file, project_root), ref.symbol);
    if (rs.status != RefResolution::Resolved) return {};

    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto sym = rt_snap->get_enhanced_symbol(rs.id);
    if (!sym) return {};
    if (sym->symbol.type != SymbolType::Method) return {};

    auto file_symbols = rt_snap->get_file_enhanced_symbols(sym->symbol.file_id);
    std::vector<SymbolID> sibling_ids;
    for (const auto& fs : file_symbols) {
        if (fs->id == sym->id) continue;
        if (fs->symbol.type != SymbolType::Method) continue;
        if (!sym->receiver_type.empty() &&
            fs->receiver_type != sym->receiver_type) {
            continue;
        }
        sibling_ids.push_back(fs->id);
    }

    absl::flat_hash_set<SymbolID> visited;
    visited.insert(sym->id);

    return hydrate_symbol_ids(*this, sibling_ids, tracker, index_,
                              remaining_tokens, project_root, format, visited);
}

std::vector<HydratedRef> ExpansionEngine::expand_tests(
    const ContextRef& ref, int remaining_tokens,
    const std::string& project_root, FormatType format) {
    if (ref.symbol.empty()) return {};

    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();

    // Strategy 1: find Test{SymbolName}
    std::string test_name = "Test" + ref.symbol;
    auto test_symbols = rt_snap->find_symbols_by_name(test_name);

    std::vector<SymbolID> test_ids;
    for (const auto& ts : test_symbols) {
        auto path = get_file_path(ts->symbol.file_id);
        if (path.find("_test.") != std::string::npos ||
            path.find("_test/") != std::string::npos ||
            path.find("test_") != std::string::npos) {
            test_ids.push_back(ts->id);
        }
    }

    // Strategy 2: find callers that are test functions
    auto rs = resolve_start(resolve_path(ref.file, project_root), ref.symbol);
    if (rs.status == RefResolution::Resolved) {
        auto sym = rt_snap->get_enhanced_symbol(rs.id);
        if (sym) {
            auto caller_ids = tracker.get_caller_symbols(sym->id);
            for (auto cid : caller_ids) {
                auto caller = rt_snap->get_enhanced_symbol(cid);
                if (!caller) continue;
                if (caller->symbol.name.substr(0, 4) != "Test") continue;
                auto path = get_file_path(caller->symbol.file_id);
                if (path.find("_test.") != std::string::npos ||
                    path.find("test_") != std::string::npos) {
                    test_ids.push_back(cid);
                }
            }
        }
    }

    absl::flat_hash_set<SymbolID> visited;
    return hydrate_symbol_ids(*this, test_ids, tracker, index_,
                              remaining_tokens, project_root, format, visited);
}

void ExpansionEngine::extract_documentation(HydratedRef& ref) {
    std::string result;
    size_t pos = 0;
    auto& src = ref.source;

    while (pos < src.size()) {
        auto nl = src.find('\n', pos);
        auto line = (nl != std::string::npos) ? src.substr(pos, nl - pos)
                                               : src.substr(pos);
        auto trimmed = line;
        auto first = trimmed.find_first_not_of(" \t");
        if (first != std::string::npos) {
            trimmed = trimmed.substr(first);
        }

        if (trimmed.substr(0, 2) == "//" || trimmed.substr(0, 2) == "/*" ||
            trimmed.substr(0, 1) == "*") {
            if (!result.empty()) result += '\n';
            result += line;
        } else if (!trimmed.empty()) {
            break;
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    if (!result.empty()) {
        ref.source = std::move(result);
    }
}

void ExpansionEngine::extract_signature_only(HydratedRef& ref) {
    size_t pos = 0;
    auto& src = ref.source;

    while (pos < src.size()) {
        auto nl = src.find('\n', pos);
        auto line = (nl != std::string::npos) ? src.substr(pos, nl - pos)
                                               : src.substr(pos);
        auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '/') {
            if (nl == std::string::npos) break;
            pos = nl + 1;
            continue;
        }
        auto trimmed = line.substr(first);
        ref.source = trimmed;
        ref.signature = trimmed;
        return;
    }
}

}  // namespace mcp
}  // namespace lci
