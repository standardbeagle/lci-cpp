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

#include <lci/analysis/side_effect_analyzer.h>
#include <lci/side_effects.h>

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

namespace {

// Directives that walk a caller/callee depth; their depth is clamped by the
// shared traversal cap (kHydrationMaxExpansionDepth) and the clamp is
// reported, never silently applied.
bool is_depth_directive(std::string_view type) {
    return type == "callers" || type == "callees";
}

// Directives that resolve exactly one hop of direct relationships: supplying
// a depth greater than 1 exceeds what they support.
bool is_direct_only_directive(std::string_view type) {
    return type == "references" || type == "dependencies" ||
           type == "implementations" || type == "interface" ||
           type == "siblings" || type == "tests";
}

// Directives that transform the selected ref in place; they take no depth.
bool is_inplace_directive(std::string_view type) {
    return type == "doc" || type == "signature" || type == "side_effects";
}

bool is_supported_directive(std::string_view type) {
    return is_depth_directive(type) || is_direct_only_directive(type) ||
           is_inplace_directive(type);
}

bool is_positive_integer(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

}  // namespace

std::string expansion_directive_error(std::string_view directive) {
    if (directive.empty()) return "empty expansion directive";
    auto colon = directive.find(':');
    std::string type(directive.substr(0, colon));
    if (type.empty()) return "expansion directive has no type";
    if (!is_supported_directive(type)) {
        return "unknown expansion directive '" + type + "'";
    }
    if (colon == std::string_view::npos) return {};  // depth defaults to 1

    std::string depth_str(directive.substr(colon + 1));
    if (depth_str.empty()) {
        return "directive '" + type + "' has an empty depth";
    }
    if (!is_positive_integer(depth_str)) {
        return "directive '" + type + "' depth '" + depth_str +
               "' is not a positive integer";
    }
    // Parse into a wider type so an absurd literal cannot overflow.
    unsigned long long v = 0;
    for (char c : depth_str) {
        v = v * 10 + static_cast<unsigned long long>(c - '0');
        if (v > 1000000000ULL) break;
    }
    if (v < 1) {
        return "directive '" + type + "' depth '" + depth_str +
               "' is not a positive integer";
    }
    if (is_inplace_directive(type)) {
        return "directive '" + type + "' does not support a depth";
    }
    if (is_direct_only_directive(type) && v > 1) {
        return "directive '" + type +
               "' is direct-only (max depth 1), got " + depth_str;
    }
    return {};
}


// -- ExpansionEngine ----------------------------------------------------------

ExpansionEngine::ExpansionEngine(MasterIndex& index) : index_(index) {}

// -- Path resolution ----------------------------------------------------------

std::string ExpansionEngine::resolve_path(const std::string& file,
                                           const std::string& project_root) const {
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

// -- Canonical identity -------------------------------------------------------

std::string ExpansionEngine::identity_key(const HydratedRef& hr,
                                          const std::string& project_root) const {
    // Normalize the file so a primary stored as a relative path and an
    // expansion resolved by SymbolID to the absolute index path collapse to the
    // same key. resolve_path leaves an already-absolute path unchanged.
    std::string path = resolve_path(hr.file, project_root);
    std::string key;
    key.reserve(path.size() + hr.symbol.size() + 24);
    key += path;
    key += '\x1f';
    key += hr.symbol;
    key += '\x1f';
    key += std::to_string(hr.lines.start);
    key += '-';
    key += std::to_string(hr.lines.end);
    return key;
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
            RefResolution::Resolved, rs.id};
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
        auto [source, lines, info, err, reason, sym_id] =
            extract_symbol_source(file_path, ref.symbol, format);
        if (!err.empty() || reason != RefResolution::Resolved) {
            return {{}, 0, err, reason};
        }
        hr.source = std::move(source);
        hr.lines = lines;
        hr.symbol_type = info.symbol_type;
        hr.signature = info.signature;
        hr.is_exported = info.is_exported;
        {
            auto snap = index_.ref_tracker().pin();
            auto sym = snap->get_enhanced_symbol(sym_id);
            populate_purity(hr, sym.get());
        }
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
    populate_purity(hr, sym.get());
    if (format == FormatType::Signatures) extract_signature_only(hr);

    int tokens = static_cast<int>(hr.source.size()) / 4;
    return {std::move(hr), tokens, {}, RefResolution::Resolved};
}

// -- apply_expansions ---------------------------------------------------------

ExpansionEngine::ExpansionResult ExpansionEngine::apply_expansions(
    const ContextRef& ref, HydratedRef& hydrated, FormatType format,
    const std::string& project_root, ExpansionTally& tally) {
    if (ref.expansions.empty()) {
        return {};
    }

    ExpansionResult out;

    for (const auto& directive : ref.expansions) {
        // Refuse an unknown / malformed / depth-exceeding directive visibly
        // rather than skipping it: a caller must learn that a requested
        // relationship was never applied.
        if (auto verr = expansion_directive_error(directive); !verr.empty()) {
            out.errors.push_back({directive, std::move(verr)});
            continue;
        }

        auto [dtype, depth] = parse_expansion_directive(directive);

        // The token budget is owned by the caller's single serialized-JSON
        // accounting; expansions bound only traversal here. Clamp each
        // directive's depth to the shared cap so a cyclic or over-deep graph
        // cannot drive the walk, and report the clamp rather than silently
        // dropping a deeper reach.
        if (depth > tally.max_depth) {
            depth = tally.max_depth;
            tally.traversal_truncated = true;
        }
        tally.relationship = dtype;

        std::vector<HydratedRef> expanded;
        if (dtype == "callers") {
            expanded = expand_callers(ref, depth, project_root, format, tally);
        } else if (dtype == "callees") {
            expanded = expand_callees(ref, depth, project_root, format, tally);
        } else if (dtype == "references") {
            expanded = expand_references(ref, project_root, format, tally);
        } else if (dtype == "dependencies") {
            expanded = expand_dependencies(ref, project_root, format, tally);
        } else if (dtype == "implementations") {
            expanded = expand_implementations(ref, project_root, format, tally);
        } else if (dtype == "interface") {
            expanded = expand_interface(ref, project_root, format, tally);
        } else if (dtype == "siblings") {
            expanded = expand_siblings(ref, project_root, format, tally);
        } else if (dtype == "tests") {
            expanded = expand_tests(ref, project_root, format, tally);
        } else if (dtype == "doc") {
            extract_documentation(hydrated);
            continue;
        } else if (dtype == "signature") {
            extract_signature_only(hydrated);
            continue;
        } else if (dtype == "side_effects") {
            request_side_effects(hydrated);
            continue;
        } else {
            continue;  // unreachable: expansion_directive_error rejected it
        }

        // Emit the new, already-deduplicated expansion refs; the caller appends
        // them to the working set and the serialized budget decides admission.
        out.expanded.insert(out.expanded.end(),
                            std::make_move_iterator(expanded.begin()),
                            std::make_move_iterator(expanded.end()));
    }

    return out;
}

// -- Expansion helpers --------------------------------------------------------

namespace {

/// Hydrate candidate SymbolIDs into NEW, deduplicated expansion refs. Each
/// target is resolved by its exact SymbolID (never re-derived by name), so an
/// expansion sibling cannot be substituted by, or made ambiguous against, a
/// same-name symbol in another file. An identity already present in
/// `tally.emitted_keys` is skipped — its source is never repeated — and recorded
/// in `tally.deduped` with the target's relationship label, so the caller folds
/// the relationship into the surviving entry's provenance. A new identity is
/// labelled with that relationship in its own provenance, inserted into
/// `emitted_keys` and consumes one unit of the shared visited-target budget;
/// once the budget is spent, traversal is marked truncated and no further
/// target is hydrated. This is the single place dedup and traversal bounds are
/// enforced across the whole working set (all primaries and all expansions),
/// rather than per directive.
///
/// `labels` (when non-empty and aligned with `ids`) names each id's explicit
/// relationship, so `dependencies` can carry "dependency:call"/"dependency:
/// import"; an empty entry falls back to the shared `tally.relationship`.
std::vector<HydratedRef> hydrate_symbol_ids(
    ExpansionEngine& engine, const std::vector<SymbolID>& ids,
    const std::vector<std::string>& labels, const std::string& project_root,
    FormatType format, ExpansionTally& tally) {
    std::vector<HydratedRef> results;
    for (size_t i = 0; i < ids.size(); ++i) {
        std::string label = (i < labels.size() && !labels[i].empty())
                                ? labels[i]
                                : tally.relationship;
        auto result = engine.hydrate_symbol_id(ids[i], format);
        if (!result.error.empty()) continue;

        std::string key = engine.identity_key(result.ref, project_root);
        if (tally.emitted_keys && tally.emitted_keys->contains(key)) {
            tally.deduped.emplace_back(std::move(key), label);
            continue;
        }
        if (tally.visited_used >= tally.visited_cap) {
            tally.traversal_truncated = true;
            break;
        }
        ++tally.visited_used;
        if (tally.emitted_keys) tally.emitted_keys->insert(std::move(key));
        if (!label.empty()) result.ref.provenance.push_back(std::move(label));
        results.push_back(std::move(result.ref));
    }
    return results;
}

// Convenience overload for the single-relationship directives (callers,
// callees, ...) whose label is the shared tally relationship.
std::vector<HydratedRef> hydrate_symbol_ids(
    ExpansionEngine& engine, const std::vector<SymbolID>& ids,
    const std::string& project_root, FormatType format, ExpansionTally& tally) {
    return hydrate_symbol_ids(engine, ids, {}, project_root, format, tally);
}

}  // namespace

std::vector<HydratedRef> ExpansionEngine::expand_callers(
    const ContextRef& ref, int depth, const std::string& project_root,
    FormatType format, ExpansionTally& tally) {
    if (ref.symbol.empty()) return {};

    auto rs = resolve_start(resolve_path(ref.file, project_root), ref.symbol);
    if (rs.status != RefResolution::Resolved) return {};

    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto sym = rt_snap->get_enhanced_symbol(rs.id);
    if (!sym) return {};

    // Level-order walk up to `depth` hops, deduped, so "callers:2" reaches
    // callers-of-callers. `seen` is the BFS dedup set (and excludes the start
    // symbol, which the caller already emitted as a primary); the shared
    // cross-set dedup happens in hydrate_symbol_ids.
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

    return hydrate_symbol_ids(*this, ordered, project_root, format, tally);
}

std::vector<HydratedRef> ExpansionEngine::expand_callees(
    const ContextRef& ref, int depth, const std::string& project_root,
    FormatType format, ExpansionTally& tally) {
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

    return hydrate_symbol_ids(*this, ordered, project_root, format, tally);
}

std::vector<HydratedRef> ExpansionEngine::expand_references(
    const ContextRef& ref, const std::string& project_root, FormatType /*format*/,
    ExpansionTally& tally) {
    if (ref.symbol.empty()) return {};

    auto rs = resolve_start(resolve_path(ref.file, project_root), ref.symbol);
    if (rs.status != RefResolution::Resolved) return {};

    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto target = rt_snap->get_enhanced_symbol(rs.id);
    if (!target) return {};

    // Incoming reference SITES of the exact selected symbol. Each site is a
    // source location; the enclosing symbol's name and line anchor it.
    auto refs = rt_snap->get_symbol_references(target->id, "incoming");
    std::sort(refs.begin(), refs.end(),
              [](const Reference& a, const Reference& b) {
                  if (a.file_id != b.file_id) return a.file_id < b.file_id;
                  if (a.line != b.line) return a.line < b.line;
                  return a.column < b.column;
              });

    std::vector<HydratedRef> results;
    for (const auto& site : refs) {
        if (site.line <= 0) continue;
        std::string file_path = get_file_path(site.file_id);
        if (file_path.empty()) continue;

        HydratedRef hr;
        hr.file = file_path;
        hr.lines = {site.line, site.line};
        if (auto enclosing = rt_snap->get_enhanced_symbol(site.source_symbol)) {
            hr.symbol = enclosing->symbol.name;
            hr.symbol_type = std::string(to_string(enclosing->symbol.type));
            hr.signature = enclosing->signature;
            hr.is_exported = enclosing->is_exported;
        }
        auto [source, err] =
            extract_source_by_lines(file_path, site.line, site.line);
        if (!err.empty()) continue;
        hr.source = std::move(source);
        // Line numbers are current-index positions, not saved selectors.
        hr.is_line_range_literal = true;
        hr.provenance.push_back("references");
        hr.provenance.push_back(std::string("reference:") +
                                std::string(to_string(site.type)));

        std::string key = identity_key(hr, project_root);
        if (tally.emitted_keys && tally.emitted_keys->contains(key)) {
            tally.deduped.emplace_back(std::move(key), "references");
            continue;
        }
        if (tally.visited_used >= tally.visited_cap) {
            tally.traversal_truncated = true;
            break;
        }
        ++tally.visited_used;
        if (tally.emitted_keys) tally.emitted_keys->insert(std::move(key));
        results.push_back(std::move(hr));
    }
    return results;
}

std::vector<HydratedRef> ExpansionEngine::expand_dependencies(
    const ContextRef& ref, const std::string& project_root, FormatType format,
    ExpansionTally& tally) {
    if (ref.symbol.empty()) return {};

    auto rs = resolve_start(resolve_path(ref.file, project_root), ref.symbol);
    if (rs.status != RefResolution::Resolved) return {};

    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto sym = rt_snap->get_enhanced_symbol(rs.id);
    if (!sym) return {};

    // Direct dependencies only: the selected symbol's resolved outgoing calls
    // and imports, each labelled with its explicit reference type. No graph
    // walk — transitive reach is deliberately out of scope.
    auto refs = rt_snap->get_symbol_references(sym->id, "outgoing");
    std::sort(refs.begin(), refs.end(),
              [](const Reference& a, const Reference& b) {
                  if (a.file_id != b.file_id) return a.file_id < b.file_id;
                  if (a.line != b.line) return a.line < b.line;
                  return a.column < b.column;
              });

    std::vector<SymbolID> ids;
    std::vector<std::string> labels;
    for (const auto& dep : refs) {
        // Only index-backed relationships are dependencies: an unresolved
        // target (an external library) carries no source to hydrate.
        if (dep.target_symbol == 0) continue;
        if (dep.type != ReferenceType::Call && dep.type != ReferenceType::Import) {
            continue;
        }
        ids.push_back(dep.target_symbol);
        labels.push_back(std::string("dependency:") +
                         std::string(to_string(dep.type)));
    }
    return hydrate_symbol_ids(*this, ids, labels, project_root, format, tally);
}

void ExpansionEngine::populate_purity(HydratedRef& hr,
                                      const EnhancedSymbol* sym) {
    hr.purity = {};
    hr.has_purity = false;
    if (sym == nullptr) {
        hr.purity.unavailability_reason = "missing_symbol";
        return;
    }
    if (sym->symbol.type != SymbolType::Function &&
        sym->symbol.type != SymbolType::Method) {
        hr.purity.unavailability_reason = "not_a_function";
        return;
    }
    if (analyzer_ == nullptr) {
        hr.purity.unavailability_reason = "analyzer_unavailable";
        return;
    }
    const SideEffectInfo* info = analyzer_->get_result(
        get_file_path(sym->symbol.file_id), sym->symbol.line,
        sym->symbol.column);
    if (info == nullptr) {
        hr.purity.unavailability_reason = "no_side_effect_record";
        return;
    }
    hr.purity.available = true;
    hr.purity.is_pure = info->is_pure;
    hr.purity.purity_level = std::string(to_string(info->purity_level));
    hr.purity.categories = categories_to_strings(info->categories);
    hr.purity.transitive_categories =
        categories_to_strings(info->transitive_categories);
    hr.purity.purity_score = info->purity_score;
    hr.purity.reasons = info->impurity_reasons;
}

void ExpansionEngine::request_side_effects(HydratedRef& hr) {
    hr.has_purity = true;
    // Symbol refs already carry a populated state (available, or a reason set
    // during hydration). A ref with no symbol identity has nothing to look up.
    if (hr.symbol.empty() && hr.purity.unavailability_reason.empty()) {
        hr.purity.unavailability_reason = "no_symbol";
    }
}

std::vector<HydratedRef> ExpansionEngine::expand_implementations(
    const ContextRef& ref, const std::string& project_root, FormatType format,
    ExpansionTally& tally) {
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

    return hydrate_symbol_ids(*this, all_ids, project_root, format, tally);
}

std::vector<HydratedRef> ExpansionEngine::expand_interface(
    const ContextRef& ref, const std::string& project_root, FormatType format,
    ExpansionTally& tally) {
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

    return hydrate_symbol_ids(*this, all_ids, project_root, format, tally);
}

std::vector<HydratedRef> ExpansionEngine::expand_siblings(
    const ContextRef& ref, const std::string& project_root, FormatType format,
    ExpansionTally& tally) {
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

    return hydrate_symbol_ids(*this, sibling_ids, project_root, format, tally);
}

std::vector<HydratedRef> ExpansionEngine::expand_tests(
    const ContextRef& ref, const std::string& project_root, FormatType format,
    ExpansionTally& tally) {
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

    return hydrate_symbol_ids(*this, test_ids, project_root, format, tally);
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
