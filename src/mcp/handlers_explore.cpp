#include <lci/mcp/handlers_explore.h>

#include <algorithm>
#include <string>
#include <vector>

#include <lci/core/reference_tracker.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_core.h>  // similar_symbol_suggestions
#include <lci/mcp/validation.h>
#include <lci/search/search_engine.h>  // relative_to_root
#include <lci/symbol.h>

namespace lci {
namespace mcp {

// -- Helpers ------------------------------------------------------------------

namespace {

/// Converts a string to lowercase.
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

/// Splits a comma-separated string into trimmed tokens.
std::vector<std::string> parse_list(const std::string& s) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start < s.size()) {
        auto end = s.find(',', start);
        if (end == std::string::npos) end = s.size();
        auto first = s.find_first_not_of(' ', start);
        if (first != std::string::npos && first < end) {
            auto last = s.find_last_not_of(' ', end - 1);
            result.push_back(s.substr(first, last - first + 1));
        }
        start = end + 1;
    }
    return result;
}

/// Returns true if the comma-separated list contains the item.
bool list_contains(const std::string& list, const std::string& item) {
    for (const auto& tok : parse_list(list)) {
        if (tok == item) return true;
    }
    return false;
}

/// Clamps an integer to a range.
int clamp_int(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

/// Guesses programming language from file extension.
std::string language_from_path(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    auto ext = to_lower(path.substr(dot));
    if (ext == ".go") return "go";
    if (ext == ".js") return "javascript";
    if (ext == ".ts") return "typescript";
    if (ext == ".jsx") return "jsx";
    if (ext == ".tsx") return "tsx";
    if (ext == ".py") return "python";
    if (ext == ".rs") return "rust";
    if (ext == ".java") return "java";
    if (ext == ".cs") return "csharp";
    if (ext == ".kt") return "kotlin";
    if (ext == ".rb") return "ruby";
    if (ext == ".swift") return "swift";
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") return "cpp";
    if (ext == ".c") return "c";
    if (ext == ".h" || ext == ".hpp") return "c/cpp";
    return "";
}

/// Parses a kind string (comma-separated) into a set of SymbolType values.
/// Returns empty set meaning "all" if kindStr is empty or "all".
std::vector<SymbolType> parse_symbol_kinds(const std::string& kind_str) {
    if (kind_str.empty() || to_lower(kind_str) == "all") return {};
    std::vector<SymbolType> kinds;
    for (const auto& k : parse_list(kind_str)) {
        auto low = to_lower(k);
        if (low == "func" || low == "fn" || low == "function")
            kinds.push_back(SymbolType::Function);
        else if (low == "type") {
            kinds.push_back(SymbolType::Type);
            kinds.push_back(SymbolType::Struct);
            kinds.push_back(SymbolType::Interface);
            kinds.push_back(SymbolType::Class);
            kinds.push_back(SymbolType::Enum);
            kinds.push_back(SymbolType::Record);
            kinds.push_back(SymbolType::Trait);
        } else if (low == "struct")
            kinds.push_back(SymbolType::Struct);
        else if (low == "interface" || low == "iface")
            kinds.push_back(SymbolType::Interface);
        else if (low == "method")
            kinds.push_back(SymbolType::Method);
        else if (low == "class" || low == "cls")
            kinds.push_back(SymbolType::Class);
        else if (low == "enum")
            kinds.push_back(SymbolType::Enum);
        else if (low == "variable" || low == "var")
            kinds.push_back(SymbolType::Variable);
        else if (low == "constant" || low == "const")
            kinds.push_back(SymbolType::Constant);
        else if (low == "field")
            kinds.push_back(SymbolType::Field);
        else if (low == "property")
            kinds.push_back(SymbolType::Property);
        else if (low == "module")
            kinds.push_back(SymbolType::Module);
        else if (low == "namespace")
            kinds.push_back(SymbolType::Namespace);
        else if (low == "constructor")
            kinds.push_back(SymbolType::Constructor);
        else if (low == "trait")
            kinds.push_back(SymbolType::Trait);
    }
    return kinds;
}

/// Returns true if the given type is in the kinds list (empty = all match).
bool kind_matches(SymbolType type, const std::vector<SymbolType>& kinds) {
    if (kinds.empty()) return true;
    return std::find(kinds.begin(), kinds.end(), type) != kinds.end();
}

/// Parses the "include" parameter into a set of section names.
/// Default includes: signature, ids.
std::vector<std::string> parse_explore_includes(const std::string& include) {
    if (include.empty()) return {"signature", "ids"};
    auto items = parse_list(include);
    for (auto& item : items) item = to_lower(item);
    for (const auto& item : items) {
        if (item == "all") {
            return {"signature", "doc", "refs", "callers",
                    "callees", "scope", "ids"};
        }
    }
    return items;
}

/// Parses the "include" parameter for inspect_symbol.
/// Default: all sections.
std::vector<std::string> parse_inspect_includes(const std::string& include) {
    if (include.empty() || to_lower(include) == "all") {
        return {"signature", "doc", "callers", "callees",
                "type_hierarchy", "scope", "refs", "annotations", "flags"};
    }
    auto items = parse_list(include);
    for (auto& item : items) item = to_lower(item);
    return items;
}

/// Returns true if the includes list contains the item.
bool includes_has(const std::vector<std::string>& includes,
                  const std::string& item) {
    return std::find(includes.begin(), includes.end(), item) != includes.end();
}

/// Decodes function flag bits to human-readable names.
std::vector<std::string> decode_function_flags(uint8_t flags) {
    std::vector<std::string> result;
    if (flags & function_flags::kAsync) result.emplace_back("async");
    if (flags & function_flags::kGenerator) result.emplace_back("generator");
    if (flags & function_flags::kMethod) result.emplace_back("method");
    if (flags & function_flags::kVariadic) result.emplace_back("variadic");
    return result;
}

/// Decodes variable flag bits to human-readable names.
std::vector<std::string> decode_variable_flags(uint8_t flags) {
    std::vector<std::string> result;
    if (flags & variable_flags::kConst) result.emplace_back("const");
    if (flags & variable_flags::kStatic) result.emplace_back("static");
    if (flags & variable_flags::kPointer) result.emplace_back("pointer");
    if (flags & variable_flags::kArray) result.emplace_back("array");
    if (flags & variable_flags::kChannel) result.emplace_back("channel");
    if (flags & variable_flags::kInterface) result.emplace_back("interface");
    return result;
}

/// Simple glob match: checks if pattern matches path or basename.
bool path_matches_glob(const std::string& path, const std::string& pattern) {
    if (pattern.empty()) return true;
    // Exact match
    if (path == pattern) return true;
    // Basename match
    auto slash = path.rfind('/');
    auto basename = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (basename == pattern) return true;
    // Suffix match (e.g., "server.go" matches "internal/mcp/server.go")
    if (path.size() > pattern.size() &&
        path.substr(path.size() - pattern.size()) == pattern &&
        (path[path.size() - pattern.size() - 1] == '/' ||
         path[path.size() - pattern.size() - 1] == '\\')) {
        return true;
    }
    return false;
}

struct SymbolWithFile {
    const EnhancedSymbol* sym;
    std::string file_path;
};

void attach_source_excerpt(nlohmann::json& j, const EnhancedSymbol& sym,
                           MasterIndex& indexer) {
    static constexpr int kMaxExcerptLines = 12;

    const int start_line = std::max(1, sym.symbol.line);
    int end_line = sym.symbol.end_line > 0 ? sym.symbol.end_line : start_line;
    if (end_line < start_line) end_line = start_line;
    const int capped_end =
        std::min(end_line, start_line + kMaxExcerptLines - 1);

    auto fc = indexer.file_content_store().get_file(sym.symbol.file_id);
    if (!fc) return;

    nlohmann::json lines = nlohmann::json::array();
    lines.get_ref<nlohmann::json::array_t&>().reserve(
        static_cast<size_t>(capped_end - start_line + 1));
    for (int line = start_line; line <= capped_end; ++line) {
        auto ref = indexer.file_content_store().get_line(sym.symbol.file_id,
                                                         line - 1);
        auto text = indexer.file_content_store().get_string(ref);
        lines.push_back({{"line", line}, {"text", std::string(text)}});
    }
    if (lines.empty()) return;

    j["source_excerpt"] =
        nlohmann::json{{"start_line", start_line},
                       {"end_line", capped_end},
                       {"truncated", capped_end < end_line},
                       {"lines", std::move(lines)}};
    j["source_hint"] =
        "source_excerpt is extracted from the indexed source. Use it for "
        "nearby code, initializer, and short body questions; read the file "
        "only if you need lines outside this excerpt.";
}

/// Builds a JSON object for a symbol in explore responses.
nlohmann::json build_explore_symbol(const EnhancedSymbol& sym,
                                    const std::string& file_path,
                                    const std::vector<std::string>& includes,
                                    ReferenceTracker& tracker) {
    nlohmann::json j;
    j["name"] = sym.symbol.name;
    j["type"] = std::string(to_string(sym.symbol.type));
    // Empty file_path = caller already names the file once at the response
    // header (browse_file); repeating it per symbol cost ~230 chars/row on
    // deep absolute paths.
    if (!file_path.empty()) j["file"] = file_path;
    j["line"] = sym.symbol.line;
    if (sym.is_exported) j["is_exported"] = true;

    if (includes_has(includes, "ids")) {
        j["object_id"] = encode_symbol_id(sym.id);
    }

    if (includes_has(includes, "signature") && !sym.signature.empty()) {
        // Declaration line only — some extractors store the whole body in
        // `signature` (Kotlin), which belongs to get_context, not a
        // per-file listing.
        auto nl = sym.signature.find('\n');
        j["signature"] = nl == std::string::npos
                             ? std::string(sym.signature)
                             : std::string(sym.signature.substr(0, nl)) + " …";
    }

    if (includes_has(includes, "doc") && !sym.doc_comment.empty()) {
        j["doc_comment"] = sym.doc_comment;
    }

    if (sym.complexity > 0) {
        j["complexity"] = sym.complexity;
    }
    if (sym.parameter_count > 0) {
        j["parameter_count"] = static_cast<int>(sym.parameter_count);
    }
    if (!sym.receiver_type.empty()) {
        j["receiver_type"] = sym.receiver_type;
    }

    if (includes_has(includes, "refs")) {
        j["incoming_refs"] = static_cast<int>(sym.incoming_refs.size());
        j["outgoing_refs"] = static_cast<int>(sym.outgoing_refs.size());
    }

    if (includes_has(includes, "callers")) {
        j["callers"] = tracker.get_caller_names(sym.id);
    }

    if (includes_has(includes, "callees")) {
        j["callees"] = tracker.get_callee_names(sym.id);
    }

    if (includes_has(includes, "scope") && !sym.scope_chain.empty()) {
        nlohmann::json chain = nlohmann::json::array();
        for (const auto& sc : sym.scope_chain) {
            chain.push_back(sc.name);
        }
        j["scope_chain"] = std::move(chain);
    }

    return j;
}

/// Builds a JSON object for inspect_symbol responses.
nlohmann::json build_inspect_result(const EnhancedSymbol& sym,
                                    const std::string& file_path,
                                    const std::vector<std::string>& includes,
                                    MasterIndex& indexer,
                                    int max_depth) {
    auto& tracker = indexer.ref_tracker();
    nlohmann::json j;
    j["name"] = sym.symbol.name;
    j["object_id"] = encode_symbol_id(sym.id);
    j["type"] = std::string(to_string(sym.symbol.type));
    j["file"] = file_path;
    j["line"] = sym.symbol.line;
    j["is_exported"] = sym.is_exported;
    j["complexity"] = sym.complexity;
    j["parameter_count"] = static_cast<int>(sym.parameter_count);

    if (!sym.receiver_type.empty()) {
        j["receiver_type"] = sym.receiver_type;
    }

    if (includes_has(includes, "signature") && !sym.signature.empty()) {
        j["signature"] = sym.signature;
    }

    if (includes_has(includes, "doc") && !sym.doc_comment.empty()) {
        j["doc_comment"] = sym.doc_comment;
    }

    if (includes_has(includes, "flags")) {
        auto ff = decode_function_flags(sym.function_flags);
        if (!ff.empty()) j["function_flags"] = std::move(ff);
        auto vf = decode_variable_flags(sym.variable_flags);
        if (!vf.empty()) j["variable_flags"] = std::move(vf);
    }

    if (includes_has(includes, "callers")) {
        j["callers"] = tracker.get_caller_names(sym.id);
    }

    if (includes_has(includes, "callees")) {
        j["callees"] = tracker.get_callee_names(sym.id);
    }

    if (includes_has(includes, "type_hierarchy")) {
        auto rels = tracker.get_type_relationships(sym.id);
        if (rels.has_relationships()) {
            auto rt_snap = tracker.pin();
            nlohmann::json th;
            auto resolve_names = [&](const std::vector<SymbolID>& ids) {
                nlohmann::json names = nlohmann::json::array();
                for (auto id : ids) {
                    auto* s = rt_snap->get_enhanced_symbol(id);
                    if (s) names.push_back(s->symbol.name);
                }
                return names;
            };
            if (!rels.implements.empty())
                th["implements"] = resolve_names(rels.implements);
            if (!rels.implemented_by.empty())
                th["implemented_by"] = resolve_names(rels.implemented_by);
            if (!rels.extends.empty())
                th["extends"] = resolve_names(rels.extends);
            if (!rels.extended_by.empty())
                th["extended_by"] = resolve_names(rels.extended_by);
            j["type_hierarchy"] = std::move(th);
        }
    }

    if (includes_has(includes, "scope") && !sym.scope_chain.empty()) {
        nlohmann::json chain = nlohmann::json::array();
        for (const auto& sc : sym.scope_chain) {
            chain.push_back(sc.name);
        }
        j["scope_chain"] = std::move(chain);
    }

    if (includes_has(includes, "refs")) {
        j["incoming_refs"] = static_cast<int>(sym.incoming_refs.size());
        j["outgoing_refs"] = static_cast<int>(sym.outgoing_refs.size());
    }

    if (includes_has(includes, "annotations") && !sym.annotations.empty()) {
        j["annotations"] = sym.annotations;
    }

    attach_source_excerpt(j, sym, indexer);

    return j;
}

/// Returns true if the symbol matches list_symbols filter criteria.
bool matches_list_filters(const EnhancedSymbol& sym,
                          const std::vector<SymbolType>& kinds,
                          const nlohmann::json& params) {
    // Anonymous symbols (empty name) are unaddressable closures/lambdas — they
    // can't be inspected or referenced by name, so they're noise in the "ls for
    // code". Under the default name sort they'd also sort first ("" precedes
    // every name), burying the real symbols. Drop them outright.
    if (sym.symbol.name.empty()) return false;

    if (!kind_matches(sym.symbol.type, kinds)) return false;

    // Exported filter
    if (params.contains("exported")) {
        bool want_exported = params["exported"].get<bool>();
        if (want_exported && !sym.is_exported) return false;
        if (!want_exported && sym.is_exported) return false;
    }

    // Name substring filter (case-insensitive)
    auto name_filter = params.value("name", "");
    if (!name_filter.empty()) {
        if (to_lower(sym.symbol.name).find(to_lower(name_filter)) ==
            std::string::npos) {
            return false;
        }
    }

    // Receiver filter
    auto receiver = params.value("receiver", "");
    if (!receiver.empty()) {
        if (to_lower(sym.receiver_type) != to_lower(receiver)) return false;
    }

    // Complexity filters
    if (params.contains("min_complexity") &&
        sym.complexity < params["min_complexity"].get<int>()) {
        return false;
    }
    if (params.contains("max_complexity") &&
        sym.complexity > params["max_complexity"].get<int>()) {
        return false;
    }

    // Parameter count filters
    if (params.contains("min_params") &&
        static_cast<int>(sym.parameter_count) <
            params["min_params"].get<int>()) {
        return false;
    }
    if (params.contains("max_params") &&
        static_cast<int>(sym.parameter_count) >
            params["max_params"].get<int>()) {
        return false;
    }

    // Flag filters
    auto flags_str = params.value("flags", "");
    if (!flags_str.empty()) {
        for (const auto& flag : parse_list(flags_str)) {
            auto low = to_lower(flag);
            if (low == "async" && !sym.is_async_func()) return false;
            if (low == "variadic" && !sym.is_variadic_func()) return false;
            if (low == "generator" && !sym.is_generator_func()) return false;
            if (low == "method" && !sym.is_method_func()) return false;
        }
    }

    return true;
}

/// Sorts symbol list by the given key.
/// All paths apply (file_path, line) tiebreakers to keep output deterministic
/// — input file iteration comes from a hash-keyed structure whose order is
/// not stable across runs (Karpathy rule 4: determinism non-negotiable).
void sort_symbols(std::vector<SymbolWithFile>& symbols,
                  const std::string& sort_by) {
    auto tiebreak = [](const SymbolWithFile& a, const SymbolWithFile& b) {
        if (a.file_path != b.file_path) return a.file_path < b.file_path;
        return a.sym->symbol.line < b.sym->symbol.line;
    };
    auto low = to_lower(sort_by);
    if (low == "complexity") {
        std::sort(symbols.begin(), symbols.end(),
                  [&](const SymbolWithFile& a, const SymbolWithFile& b) {
                      if (a.sym->complexity != b.sym->complexity)
                          return a.sym->complexity > b.sym->complexity;
                      return tiebreak(a, b);
                  });
    } else if (low == "refs") {
        std::sort(symbols.begin(), symbols.end(),
                  [&](const SymbolWithFile& a, const SymbolWithFile& b) {
                      auto ar = a.sym->incoming_refs.size();
                      auto br = b.sym->incoming_refs.size();
                      if (ar != br) return ar > br;
                      return tiebreak(a, b);
                  });
    } else if (low == "line") {
        std::sort(symbols.begin(), symbols.end(),
                  [](const SymbolWithFile& a, const SymbolWithFile& b) {
                      if (a.file_path != b.file_path)
                          return a.file_path < b.file_path;
                      return a.sym->symbol.line < b.sym->symbol.line;
                  });
    } else if (low == "params") {
        std::sort(symbols.begin(), symbols.end(),
                  [&](const SymbolWithFile& a, const SymbolWithFile& b) {
                      if (a.sym->parameter_count != b.sym->parameter_count)
                          return a.sym->parameter_count >
                                 b.sym->parameter_count;
                      return tiebreak(a, b);
                  });
    } else {
        // Default: sort by name, then file_path, then line.
        std::sort(symbols.begin(), symbols.end(),
                  [&](const SymbolWithFile& a, const SymbolWithFile& b) {
                      if (a.sym->symbol.name != b.sym->symbol.name)
                          return a.sym->symbol.name < b.sym->symbol.name;
                      return tiebreak(a, b);
                  });
    }
}

/// Sorts enhanced symbol pointers by the given key.
void sort_enhanced_symbols(
    std::vector<const EnhancedSymbol*>& symbols,
    const std::string& sort_by) {
    auto low = to_lower(sort_by);
    if (low == "name") {
        std::sort(symbols.begin(), symbols.end(),
                  [](const EnhancedSymbol* a, const EnhancedSymbol* b) {
                      return a->symbol.name < b->symbol.name;
                  });
    } else if (low == "type") {
        std::sort(symbols.begin(), symbols.end(),
                  [](const EnhancedSymbol* a, const EnhancedSymbol* b) {
                      return to_string(a->symbol.type) <
                             to_string(b->symbol.type);
                  });
    } else if (low == "complexity") {
        std::sort(symbols.begin(), symbols.end(),
                  [](const EnhancedSymbol* a, const EnhancedSymbol* b) {
                      return a->complexity > b->complexity;
                  });
    } else if (low == "refs") {
        std::sort(symbols.begin(), symbols.end(),
                  [](const EnhancedSymbol* a, const EnhancedSymbol* b) {
                      return a->incoming_refs.size() >
                             b->incoming_refs.size();
                  });
    } else {
        // Default: sort by line
        std::sort(symbols.begin(), symbols.end(),
                  [](const EnhancedSymbol* a, const EnhancedSymbol* b) {
                      return a->symbol.line < b->symbol.line;
                  });
    }
}

}  // namespace

// -- handle_list_symbols ------------------------------------------------------

ToolResult handle_list_symbols(const nlohmann::json& params,
                               MasterIndex& indexer) {
    // kind defaults to "all" — benchmark traces showed agents calling with
    // just a file filter and bouncing off a required-param error.
    auto kind_str = params.value("kind", "all");
    if (kind_str.empty()) kind_str = "all";

    auto& tracker = indexer.ref_tracker();
    auto rt_snap = tracker.pin();
    auto kinds = parse_symbol_kinds(kind_str);
    auto includes = parse_explore_includes(params.value("include", ""));

    int max_results = clamp_int(params.value("max", 50), 1, 500);
    int offset = params.value("offset", 0);
    if (offset < 0) offset = 0;

    auto file_filter = params.value("file", "");
    auto sort_by = params.value("sort", "name");

    // Collect matching symbols. Paths are emitted root-relative; the glob
    // filter accepts both forms (agents paste either).
    const std::string& proj_root = indexer.config().project.root;
    auto file_ids = indexer.get_all_file_ids();
    std::vector<SymbolWithFile> all_symbols;

    for (auto fid : file_ids) {
        auto file_path = indexer.get_file_path(fid);
        if (file_path.empty()) continue;
        std::string rel(relative_to_root(file_path, proj_root));

        if (!file_filter.empty() &&
            !path_matches_glob(file_path, file_filter) &&
            !path_matches_glob(rel, file_filter)) {
            continue;
        }

        auto symbols = rt_snap->get_file_enhanced_symbols(fid);
        for (const auto* sym : symbols) {
            if (sym && matches_list_filters(*sym, kinds, params)) {
                all_symbols.push_back({sym, rel});
            }
        }
    }

    int total = static_cast<int>(all_symbols.size());

    sort_symbols(all_symbols, sort_by);

    // Apply offset/limit
    if (offset > 0 && offset < total) {
        all_symbols.erase(all_symbols.begin(),
                          all_symbols.begin() + offset);
    } else if (offset >= total) {
        all_symbols.clear();
    }
    if (static_cast<int>(all_symbols.size()) > max_results) {
        all_symbols.resize(static_cast<size_t>(max_results));
    }

    // Build response
    nlohmann::json symbols_json = nlohmann::json::array();
    symbols_json.get_ref<nlohmann::json::array_t&>().reserve(all_symbols.size());
    for (const auto& swf : all_symbols) {
        symbols_json.push_back(
            build_explore_symbol(*swf.sym, swf.file_path, includes, tracker));
    }

    nlohmann::json response;
    response["symbols"] = std::move(symbols_json);
    response["total"] = total;
    response["showing"] = static_cast<int>(all_symbols.size());
    response["has_more"] = total > offset + static_cast<int>(all_symbols.size());

    return make_json_response(response);
}

// -- handle_inspect_symbol ----------------------------------------------------

ToolResult handle_inspect_symbol(const nlohmann::json& params,
                                 MasterIndex& indexer) {
    auto name = params.value("name", "");
    auto id_str = params.value("id", "");

    if (name.empty() && id_str.empty()) {
        return make_error_response(
            "inspect_symbol",
            "either 'name' or 'id' parameter is required");
    }

    auto& tracker = indexer.ref_tracker();
    auto rt_snap = tracker.pin();
    auto includes = parse_inspect_includes(params.value("include", ""));
    int max_depth = clamp_int(params.value("max_depth", 3), 1, 10);

    std::vector<const EnhancedSymbol*> matched;

    // Lookup by ID
    if (!id_str.empty()) {
        for (const auto& token : parse_list(id_str)) {
            auto decoded = decode_symbol_id(token);
            if (decoded.has_value()) {
                auto* sym = rt_snap->get_enhanced_symbol(decoded.value());
                if (sym) matched.push_back(sym);
            }
        }
    }

    // Lookup by name (fallback)
    if (!name.empty() && matched.empty()) {
        matched = rt_snap->find_symbols_by_name(name);
    }

    // Apply file/type disambiguation
    auto file_filter = params.value("file", "");
    auto type_filter = params.value("type", "");
    if (!file_filter.empty() || !type_filter.empty()) {
        auto type_kinds = parse_symbol_kinds(type_filter);
        std::vector<const EnhancedSymbol*> filtered;
        for (const auto* sym : matched) {
            if (!file_filter.empty()) {
                auto fp = indexer.get_file_path(sym->symbol.file_id);
                if (!path_matches_glob(fp, file_filter) &&
                    !path_matches_glob(
                        std::string(relative_to_root(
                            fp, indexer.config().project.root)),
                        file_filter)) {
                    continue;
                }
            }
            if (!type_kinds.empty() &&
                !kind_matches(sym->symbol.type, type_kinds)) {
                continue;
            }
            filtered.push_back(sym);
        }
        matched = std::move(filtered);
    }

    // Build results (root-relative paths, matching search output)
    nlohmann::json symbols_json = nlohmann::json::array();
    symbols_json.get_ref<nlohmann::json::array_t&>().reserve(matched.size());
    for (const auto* sym : matched) {
        auto fp = indexer.get_file_path(sym->symbol.file_id);
        std::string rel(relative_to_root(fp, indexer.config().project.root));
        symbols_json.push_back(
            build_inspect_result(*sym, rel, includes, indexer, max_depth));
    }

    nlohmann::json response;
    response["symbols"] = std::move(symbols_json);
    response["count"] = static_cast<int>(matched.size());

    // Empty lookup fails loud: hint + fuzzy near-miss suggestions (typo'd
    // names self-correct in one round trip; bare {count:0} teaches nothing).
    if (matched.empty()) {
        response["hint"] = name.empty()
                               ? "no symbol resolved from id '" + id_str + "'"
                               : "no symbol named '" + name +
                                     "' in the index. Check similar_symbols "
                                     "below, or use search to locate it.";
        if (!name.empty()) {
            auto sims = similar_symbol_suggestions(*rt_snap, name);
            if (!sims.empty()) response["similar_symbols"] = std::move(sims);
        }
    }

    return make_json_response(response);
}

// -- handle_browse_file -------------------------------------------------------

ToolResult handle_browse_file(const nlohmann::json& params,
                              MasterIndex& indexer) {
    // `path` is an accepted alias for `file` — search emits root-relative
    // paths agents paste across tools, and search itself scopes with path=.
    auto file_pattern = params.value("file", "");
    if (file_pattern.empty()) file_pattern = params.value("path", "");
    int file_id_param = params.value("file_id", 0);

    if (file_pattern.empty() && file_id_param == 0) {
        return make_error_response(
            "browse_file",
            "either 'file' or 'file_id' parameter is required");
    }

    auto& tracker = indexer.ref_tracker();
    auto rt_snap = tracker.pin();

    // Find the target file
    FileID target_fid = 0;
    std::string target_path;
    bool found = false;

    if (file_id_param > 0) {
        target_fid = static_cast<FileID>(file_id_param);
        target_path = indexer.get_file_path(target_fid);
        if (!target_path.empty()) found = true;
    }

    if (!found && !file_pattern.empty()) {
        const std::string& proj_root = indexer.config().project.root;
        auto file_ids = indexer.get_all_file_ids();
        for (auto fid : file_ids) {
            auto fp = indexer.get_file_path(fid);
            if (fp.empty()) continue;
            // Search results emit root-relative paths — accept both forms.
            if (path_matches_glob(fp, file_pattern) ||
                path_matches_glob(
                    std::string(relative_to_root(fp, proj_root)),
                    file_pattern)) {
                target_fid = fid;
                target_path = fp;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        nlohmann::json err;
        err["success"] = false;
        err["error"] = "file not found: " + file_pattern;
        err["operation"] = "browse_file";
        err["hint"] =
            "Use find_files to locate the correct path, or provide "
            "file_id from search results";
        return {dump_json_lossy(err), true};
    }

    auto kinds = parse_symbol_kinds(params.value("kind", ""));
    auto includes = parse_explore_includes(params.value("include", ""));
    int max_results = clamp_int(params.value("max", 100), 1, 1000);
    auto sort_by = params.value("sort", "line");

    // Get symbols for the file
    auto symbols = rt_snap->get_file_enhanced_symbols(target_fid);
    std::vector<const EnhancedSymbol*> filtered;
    for (const auto* sym : symbols) {
        if (!sym) continue;
        if (!kind_matches(sym->symbol.type, kinds)) continue;

        if (params.contains("exported")) {
            bool want = params["exported"].get<bool>();
            if (want && !sym->is_exported) continue;
            if (!want && sym->is_exported) continue;
        }

        filtered.push_back(sym);
    }

    int total = static_cast<int>(filtered.size());

    sort_enhanced_symbols(filtered, sort_by);

    if (static_cast<int>(filtered.size()) > max_results) {
        filtered.resize(static_cast<size_t>(max_results));
    }

    // Build response. The file is named ONCE in the header (root-relative);
    // per-symbol rows omit it (empty file_path arg).
    static const std::string kNoPerRowFile;
    nlohmann::json symbols_json = nlohmann::json::array();
    symbols_json.get_ref<nlohmann::json::array_t&>().reserve(filtered.size());
    for (const auto* sym : filtered) {
        symbols_json.push_back(
            build_explore_symbol(*sym, kNoPerRowFile, includes, tracker));
    }

    nlohmann::json response;
    response["file"]["path"] = std::string(relative_to_root(
        target_path, indexer.config().project.root));
    response["file"]["file_id"] = static_cast<int>(target_fid);
    response["file"]["language"] = language_from_path(target_path);
    response["symbols"] = std::move(symbols_json);
    response["total"] = total;
    if (total > 0) {
        response["hint"] =
            "This outline is extracted from source. For symbol names, types, "
            "signatures, and line numbers, answer from this response; read "
            "the file only if you need code body lines.";
    }

    // Optional stats
    if (params.value("show_stats", false)) {
        int func_count = 0;
        int type_count = 0;
        int exported_count = 0;
        int total_complexity = 0;
        int complexity_count = 0;
        int max_complexity = 0;

        for (const auto* sym : symbols) {
            if (!sym) continue;
            if (sym->is_exported) ++exported_count;
            if (sym->symbol.type == SymbolType::Function ||
                sym->symbol.type == SymbolType::Method) {
                ++func_count;
                if (sym->complexity > 0) {
                    total_complexity += sym->complexity;
                    ++complexity_count;
                    if (sym->complexity > max_complexity)
                        max_complexity = sym->complexity;
                }
            } else if (sym->symbol.type == SymbolType::Type ||
                       sym->symbol.type == SymbolType::Struct ||
                       sym->symbol.type == SymbolType::Interface ||
                       sym->symbol.type == SymbolType::Class ||
                       sym->symbol.type == SymbolType::Enum) {
                ++type_count;
            }
        }

        nlohmann::json stats;
        stats["symbol_count"] = static_cast<int>(symbols.size());
        stats["function_count"] = func_count;
        stats["type_count"] = type_count;
        stats["exported_count"] = exported_count;
        stats["max_complexity"] = max_complexity;
        if (complexity_count > 0) {
            stats["avg_complexity"] =
                static_cast<double>(total_complexity) /
                static_cast<double>(complexity_count);
        }
        response["stats"] = std::move(stats);
    }

    return make_json_response(response);
}

// -- register_explore_handlers ------------------------------------------------

void register_explore_handlers(McpServer& server, MasterIndex* indexer) {
    server.add_tool(
        {"list_symbols",
         "📋 Enumerate and filter symbols in the index. Like 'ls' for "
         "code: list all functions, types, methods with filtering by kind, "
         "file, complexity, params, exported status, and more. Use to "
         "discover what's in the codebase without searching. Returned "
         "signatures, files, line numbers, ids, and refs are source evidence; "
         "do not read files just to confirm them. See 'info list_symbols'.",
         {{"kind", "string",
           "REQUIRED: Symbol kinds (comma-separated): func, type, struct, "
           "interface, method, class, enum, variable, constant, all. "
           "Aliases: fn, var, const, cls, iface",
           ""},
          {"file", "string",
           "Glob pattern for file path filter (e.g., "
           "'internal/mcp/*.go')",
           ""},
          {"exported", "boolean",
           "true=exported only, false=unexported only, omit=all", ""},
          {"name", "string",
           "Substring filter on symbol name (case-insensitive)", ""},
          {"receiver", "string",
           "Filter methods by receiver type (e.g., 'Server')", ""},
          {"min_complexity", "integer", "Minimum cyclomatic complexity", ""},
          {"max_complexity", "integer", "Maximum cyclomatic complexity", ""},
          {"min_params", "integer", "Minimum parameter count", ""},
          {"max_params", "integer", "Maximum parameter count", ""},
          {"flags", "string",
           "Comma-separated flags: async, variadic, generator, method", ""},
          {"sort", "string",
           "Sort by: name (default), complexity, refs, line, params", ""},
          {"max", "integer", "Max results (default: 50, max: 500)", ""},
          {"offset", "integer", "Pagination offset", ""},
          {"include", "string",
           "Comma-separated extras: signature, doc, refs, callers, callees, "
           "scope, ids, all. Default: signature,ids",
           ""}},
         {"kind"}},
        [indexer](const nlohmann::json& p) -> ToolResult {
            if (!indexer) {
                return make_error_response("list_symbols",
                                           "index not available");
            }
            return handle_list_symbols(p, *indexer);
        });

    server.add_tool(
        {"inspect_symbol",
         "🔎 Deep inspect a single symbol. Given a name or object ID, "
         "returns all metadata: signature, doc, complexity, callers, "
         "callees, type hierarchy, scope chain, flags, and a bounded "
         "source_excerpt. Treat this output as the source-backed answer; read "
         "the file only if you need lines outside source_excerpt. See 'info "
         "inspect_symbol'.",
         {{"name", "string",
           "Symbol name (exact match; may return multiple if ambiguous)",
           ""},
          {"id", "string", "Object ID from search/list_symbols results", ""},
          {"file", "string", "File path pattern to disambiguate", ""},
          {"type", "string",
           "Symbol type to disambiguate (e.g., 'function', 'struct')", ""},
          {"include", "string",
           "Sections: signature, doc, callers, callees, type_hierarchy, "
           "scope, refs, annotations, flags, all. Default: all",
           ""},
          {"max_depth", "integer",
           "Max depth for hierarchy traversal (default: 3)", ""}},
         {}},
        [indexer](const nlohmann::json& p) -> ToolResult {
            if (!indexer) {
                return make_error_response("inspect_symbol",
                                           "index not available");
            }
            return handle_inspect_symbol(p, *indexer);
        });

    server.add_tool(
        {"browse_file",
         "📂 Browse all symbols in a file - the outline view. Shows the "
         "complete symbol table for a specific file with filtering, "
         "sorting, optional imports and stats. The outline is source-derived; "
         "do not open the file merely to verify symbol names, signatures, or "
         "line numbers already shown. See 'info browse_file'.",
         {{"file", "string",
           "File path (exact, suffix, or glob match)", ""},
          {"path", "string", "Alias for 'file'", ""},
          {"file_id", "integer", "File ID (alternative to path)", ""},
          {"kind", "string",
           "Filter by symbol kinds (same as list_symbols)", ""},
          {"exported", "boolean", "Visibility filter", ""},
          {"sort", "string",
           "Sort by: line (default), name, type, complexity, refs", ""},
          {"max", "integer", "Max symbols (default: 100)", ""},
          {"include", "string",
           "Same as list_symbols. Default: signature,ids", ""},
          {"show_imports", "boolean", "Include import list", ""},
          {"show_stats", "boolean",
           "Include file-level statistics (symbol counts, avg complexity)",
           ""}},
         {}},
        [indexer](const nlohmann::json& p) -> ToolResult {
            if (!indexer) {
                return make_error_response("browse_file",
                                           "index not available");
            }
            return handle_browse_file(p, *indexer);
        });
}

}  // namespace mcp
}  // namespace lci
