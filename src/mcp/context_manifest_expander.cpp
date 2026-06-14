#include <lci/mcp/context_manifest_expander.h>

#include <algorithm>
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
    if (file.empty()) return {};
    if (!file.empty() && file[0] == '/') return file;
    if (project_root.empty()) return file;
    return project_root + "/" + file;
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
    auto content = store.get_content(fid);
    if (content.empty()) {
        return {{}, "file content not found: " + file_path};
    }

    auto* offsets = store.get_line_offsets(fid);
    if (!offsets || offsets->empty()) {
        return {{}, "empty file: " + file_path};
    }

    int line_count = static_cast<int>(offsets->size());

    if (start_line < 1 || start_line > line_count) {
        return {{}, "start line " + std::to_string(start_line) +
                    " out of range (file has " + std::to_string(line_count) +
                    " lines)"};
    }
    if (end_line < start_line) end_line = start_line;
    if (end_line > line_count) end_line = line_count;

    // offsets are 0-based byte offsets; lines are 1-indexed
    auto begin_off = (*offsets)[static_cast<size_t>(start_line - 1)];
    size_t end_off;
    if (end_line < line_count) {
        end_off = (*offsets)[static_cast<size_t>(end_line)];
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

ExpansionEngine::ExtractResult ExpansionEngine::extract_symbol_source(
    const std::string& file_path, const std::string& symbol_name,
    const LineRange* line_hint, FormatType /*format*/) {
    // If we have a line hint, use it directly
    if (line_hint && line_hint->start > 0) {
        auto [source, err] =
            extract_source_by_lines(file_path, line_hint->start, line_hint->end);
        if (!err.empty()) {
            return {{}, {}, {}, err};
        }
        SymbolInfo info;
        info.start_line = line_hint->start;
        info.end_line = line_hint->end;
        auto nl = source.find('\n');
        if (nl != std::string::npos) {
            info.signature = source.substr(0, nl);
        } else {
            info.signature = source;
        }
        // Trim leading/trailing whitespace from signature
        auto first = info.signature.find_first_not_of(" \t");
        if (first != std::string::npos) {
            info.signature = info.signature.substr(first);
        }
        return {std::move(source), *line_hint, std::move(info), {}};
    }

    // Search for the symbol by name in the reference tracker
    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto* sym = rt_snap->find_symbol_by_name(symbol_name);
    if (!sym) {
        return {{}, {}, {}, "symbol " + symbol_name + " not found"};
    }

    // Check file matches
    auto sym_path = get_file_path(sym->symbol.file_id);
    auto fid = index_.path_to_id(file_path);
    if (fid != 0 && sym->symbol.file_id != fid) {
        // Try finding in the specified file
        auto* file_sym =
            rt_snap->find_symbol_by_file_and_name(fid, symbol_name);
        if (file_sym) {
            sym = file_sym;
            sym_path = file_path;
        }
    }

    int start = sym->symbol.line;
    int end = sym->symbol.end_line;
    if (end < start) end = start;

    auto [source, err] = extract_source_by_lines(
        sym_path.empty() ? file_path : sym_path, start, end);
    if (!err.empty()) {
        return {{}, {}, {}, err};
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

    return {std::move(source), {start, end}, std::move(info), {}};
}

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

    if (!ref.symbol.empty()) {
        // Case 1: Symbol name provided
        const LineRange* hint =
            ref.has_line_range ? &ref.line_range : nullptr;
        auto [source, lines, info, err] =
            extract_symbol_source(file_path, ref.symbol, hint, format);
        if (!err.empty()) {
            return {{}, 0, err};
        }
        hr.source = std::move(source);
        hr.lines = lines;
        hr.symbol_type = info.symbol_type;
        hr.signature = info.signature;
        hr.is_exported = info.is_exported;
    } else if (ref.has_line_range) {
        // Case 2: Only line range
        auto [source, err] = extract_source_by_lines(
            file_path, ref.line_range.start, ref.line_range.end);
        if (!err.empty()) {
            return {{}, 0, err};
        }
        hr.source = std::move(source);
        hr.lines = ref.line_range;
    } else {
        return {{}, 0, "reference must have either symbol name or line range"};
    }

    int tokens = static_cast<int>(hr.source.size()) / 4;
    return {std::move(hr), tokens, {}};
}

// -- apply_expansions ---------------------------------------------------------

ExpansionEngine::ExpansionResult ExpansionEngine::apply_expansions(
    const ContextRef& ref, HydratedRef& hydrated, FormatType format,
    int remaining_tokens, const std::string& project_root) {
    if (ref.expansions.empty()) {
        return {0, {}};
    }

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
        // Store expanded refs is skipped here since HydratedRef doesn't have
        // an Expanded map in the C++ types. The expanded refs are returned
        // as part of the hydrated context warnings/stats instead.
        // In a future iteration, HydratedRef could be extended with a map.
        (void)expanded;
    }

    return {total_tokens, {}};
}

// -- Expansion helpers --------------------------------------------------------

namespace {

/// Helper to hydrate a list of symbol IDs into HydratedRefs.
std::vector<HydratedRef> hydrate_symbol_ids(
    ExpansionEngine& engine, const std::vector<SymbolID>& ids,
    ReferenceTracker& tracker, MasterIndex& index, int remaining_tokens,
    const std::string& project_root, FormatType format,
    absl::flat_hash_set<SymbolID>& visited) {
    std::vector<HydratedRef> results;
    int total_tokens = 0;
    auto rt_snap = tracker.pin();

    for (auto id : ids) {
        if (total_tokens >= remaining_tokens) break;
        if (visited.contains(id)) continue;
        visited.insert(id);

        auto* sym = rt_snap->get_enhanced_symbol(id);
        if (!sym) continue;

        auto file_path = index.get_file_path(sym->symbol.file_id);
        if (file_path.empty()) continue;

        ContextRef cr;
        cr.file = file_path;
        cr.symbol = sym->symbol.name;
        cr.line_range = {sym->symbol.line, sym->symbol.end_line};
        cr.has_line_range = true;

        auto result = engine.hydrate_reference(cr, format, project_root);
        if (!result.error.empty()) continue;

        total_tokens += result.tokens;
        results.push_back(std::move(result.ref));
    }

    return results;
}

}  // namespace

std::vector<HydratedRef> ExpansionEngine::expand_callers(
    const ContextRef& ref, int /*depth*/, int remaining_tokens,
    const std::string& project_root, FormatType format) {
    if (ref.symbol.empty()) return {};

    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto* sym = rt_snap->find_symbol_by_name(ref.symbol);
    if (!sym) return {};

    auto caller_ids = tracker.get_caller_symbols(sym->id);
    absl::flat_hash_set<SymbolID> visited;
    visited.insert(sym->id);

    return hydrate_symbol_ids(*this, caller_ids, tracker, index_,
                              remaining_tokens, project_root, format, visited);
}

std::vector<HydratedRef> ExpansionEngine::expand_callees(
    const ContextRef& ref, int /*depth*/, int remaining_tokens,
    const std::string& project_root, FormatType format) {
    if (ref.symbol.empty()) return {};

    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto* sym = rt_snap->find_symbol_by_name(ref.symbol);
    if (!sym) return {};

    auto callee_ids = tracker.get_callee_symbols(sym->id);
    absl::flat_hash_set<SymbolID> visited;
    visited.insert(sym->id);

    return hydrate_symbol_ids(*this, callee_ids, tracker, index_,
                              remaining_tokens, project_root, format, visited);
}

std::vector<HydratedRef> ExpansionEngine::expand_implementations(
    const ContextRef& ref, int remaining_tokens,
    const std::string& project_root, FormatType format) {
    if (ref.symbol.empty()) return {};

    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto symbols = rt_snap->find_symbols_by_name(ref.symbol);
    if (symbols.empty()) return {};

    // Prefer interface symbols
    const EnhancedSymbol* target = nullptr;
    for (auto* s : symbols) {
        if (s->symbol.type == SymbolType::Interface) {
            target = s;
            break;
        }
        if (s->symbol.type == SymbolType::Class ||
            s->symbol.type == SymbolType::Struct ||
            s->symbol.type == SymbolType::Type) {
            if (!target) target = s;
        }
    }
    if (!target) target = symbols[0];

    auto impl_ids = tracker.get_implementors(target->id);
    auto derived_ids = tracker.get_derived_types(target->id);

    // Combine
    std::vector<SymbolID> all_ids;
    all_ids.reserve(impl_ids.size() + derived_ids.size());
    all_ids.insert(all_ids.end(), impl_ids.begin(), impl_ids.end());
    all_ids.insert(all_ids.end(), derived_ids.begin(), derived_ids.end());

    absl::flat_hash_set<SymbolID> visited;
    visited.insert(target->id);

    return hydrate_symbol_ids(*this, all_ids, tracker, index_,
                              remaining_tokens, project_root, format, visited);
}

std::vector<HydratedRef> ExpansionEngine::expand_interface(
    const ContextRef& ref, int remaining_tokens,
    const std::string& project_root, FormatType format) {
    if (ref.symbol.empty()) return {};

    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto symbols = rt_snap->find_symbols_by_name(ref.symbol);
    if (symbols.empty()) return {};

    // Prefer concrete types
    const EnhancedSymbol* target = nullptr;
    for (auto* s : symbols) {
        if (s->symbol.type == SymbolType::Class ||
            s->symbol.type == SymbolType::Struct ||
            s->symbol.type == SymbolType::Type) {
            target = s;
            break;
        }
    }
    if (!target) target = symbols[0];

    auto iface_ids = tracker.get_implemented_interfaces(target->id);
    auto base_ids = tracker.get_base_types(target->id);

    std::vector<SymbolID> all_ids;
    all_ids.reserve(iface_ids.size() + base_ids.size());
    all_ids.insert(all_ids.end(), iface_ids.begin(), iface_ids.end());
    all_ids.insert(all_ids.end(), base_ids.begin(), base_ids.end());

    absl::flat_hash_set<SymbolID> visited;
    visited.insert(target->id);

    return hydrate_symbol_ids(*this, all_ids, tracker, index_,
                              remaining_tokens, project_root, format, visited);
}

std::vector<HydratedRef> ExpansionEngine::expand_siblings(
    const ContextRef& ref, int remaining_tokens,
    const std::string& project_root, FormatType format) {
    if (ref.symbol.empty()) return {};

    auto& tracker = index_.ref_tracker();
    auto rt_snap = tracker.pin();
    auto* sym = rt_snap->find_symbol_by_name(ref.symbol);
    if (!sym) return {};
    if (sym->symbol.type != SymbolType::Method) return {};

    auto file_symbols = rt_snap->get_file_enhanced_symbols(sym->symbol.file_id);
    std::vector<SymbolID> sibling_ids;
    for (auto* fs : file_symbols) {
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
    for (auto* ts : test_symbols) {
        auto path = get_file_path(ts->symbol.file_id);
        if (path.find("_test.") != std::string::npos ||
            path.find("_test/") != std::string::npos ||
            path.find("test_") != std::string::npos) {
            test_ids.push_back(ts->id);
        }
    }

    // Strategy 2: find callers that are test functions
    auto* sym = rt_snap->find_symbol_by_name(ref.symbol);
    if (sym) {
        auto caller_ids = tracker.get_caller_symbols(sym->id);
        for (auto cid : caller_ids) {
            auto* caller = rt_snap->get_enhanced_symbol(cid);
            if (!caller) continue;
            if (caller->symbol.name.substr(0, 4) != "Test") continue;
            auto path = get_file_path(caller->symbol.file_id);
            if (path.find("_test.") != std::string::npos ||
                path.find("test_") != std::string::npos) {
                test_ids.push_back(cid);
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
