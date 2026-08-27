#include <lci/server/server.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <thread>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include <lci/core/reference_tracker.h>
#include <lci/core/text.h>
#include <lci/pagination.h>
#include <lci/file_info.h>
#include <lci/git/analyzer.h>
#include <lci/git/provider.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/language_map.h>
#include <lci/search/search_engine.h>
#include <lci/search/search_options.h>
#include <lci/server/client.h>
#include <lci/server/request_decode.h>
#include <lci/version.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#else
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <fstream>

namespace lci {


namespace {

using KindSet = std::vector<SymbolType>;

KindSet parse_symbol_kinds(const std::string& kind_str) {
    if (kind_str.empty() || kind_str == "all") {
        return {};
    }
    KindSet result;
    std::string token;
    auto flush = [&] {
        if (token.empty()) return;
        // Normalize to lowercase
        std::string low;
        low.reserve(token.size());
        for (char c : token) {
            low.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        }
        if (low == "func" || low == "fn" || low == "function") {
            result.push_back(SymbolType::Function);
        } else if (low == "type") {
            result.push_back(SymbolType::Type);
            result.push_back(SymbolType::Struct);
            result.push_back(SymbolType::Interface);
            result.push_back(SymbolType::Class);
            result.push_back(SymbolType::Enum);
            result.push_back(SymbolType::Record);
            result.push_back(SymbolType::Trait);
        } else if (low == "struct") {
            result.push_back(SymbolType::Struct);
        } else if (low == "interface" || low == "iface") {
            result.push_back(SymbolType::Interface);
        } else if (low == "method") {
            result.push_back(SymbolType::Method);
        } else if (low == "class" || low == "cls") {
            result.push_back(SymbolType::Class);
        } else if (low == "enum") {
            result.push_back(SymbolType::Enum);
        } else if (low == "variable" || low == "var") {
            result.push_back(SymbolType::Variable);
        } else if (low == "constant" || low == "const") {
            result.push_back(SymbolType::Constant);
        } else if (low == "field") {
            result.push_back(SymbolType::Field);
        }
        token.clear();
    };

    for (char c : kind_str) {
        if (c == ',') {
            flush();
        } else if (c != ' ') {
            token.push_back(c);
        }
    }
    flush();
    return result;
}

bool kind_matches(SymbolType st, const KindSet& kinds) {
    if (kinds.empty()) return true;
    return std::find(kinds.begin(), kinds.end(), st) != kinds.end();
}

// Classifies programming language from file extension via the central
// ext->language table (lci::language_map). Unknown/extensionless paths report
// "" (empty), matching Go's httpLanguageFromPath default and the field-omission
// contract downstream.

}  // namespace

// -- Endpoint: /list-symbols --------------------------------------------------

void IndexServer::handle_list_symbols(const httplib::Request& req,
                                       httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    auto kind_str = body.value("kind", "");
    auto kinds = parse_symbol_kinds(kind_str);
    auto file_filter = body.value("file", "");
    auto name_filter = body.value("name", "");
    auto receiver_filter = body.value("receiver", "");
    std::optional<bool> exported_filter;
    if (body.contains("exported") && !body["exported"].is_null()) {
        exported_filter = body["exported"].get<bool>();
    }
    std::optional<int> min_complexity, max_complexity;
    std::optional<int> min_params, max_params;
    if (body.contains("min_complexity") && !body["min_complexity"].is_null()) {
        min_complexity = body["min_complexity"].get<int>();
    }
    if (body.contains("max_complexity") && !body["max_complexity"].is_null()) {
        max_complexity = body["max_complexity"].get<int>();
    }
    if (body.contains("min_params") && !body["min_params"].is_null()) {
        min_params = body["min_params"].get<int>();
    }
    if (body.contains("max_params") && !body["max_params"].is_null()) {
        max_params = body["max_params"].get<int>();
    }
    const auto page = normalize_page(body);
    const int max_results = page.max;
    const int offset = page.offset;
    const auto sort_key = body.value("sort", "");

    auto all_file_ids = indexer_->get_all_file_ids();
    // Sort by file_id ascending for deterministic output ordering.
    // get_all_file_ids() iterates a hash map and returns ids in arbitrary
    // order; without sorting, list-symbols output is non-reproducible across
    // runs and diverges from the Go reference (which iterates files in
    // insertion / file_id order).
    std::sort(all_file_ids.begin(), all_file_ids.end());

    // Collect ALL matching symbols before paging. The sort (when requested)
    // must run over the full filtered set: sorting a pre-capped page turns
    // "top N by complexity" into "an arbitrary page, reordered" — the same
    // cap-before-score defect class fixed in find_files (1d00e11).
    struct Row {
        ReferenceTracker::Snapshot::SymbolHandle sym;
        uint32_t path_idx;
    };
    std::vector<Row> rows;
    std::vector<std::string> row_paths;  // One entry per contributing file.

    auto rt_snap = indexer_->ref_tracker().pin();
    for (auto fid : all_file_ids) {
        auto file_path = indexer_->get_file_path(fid);
        if (file_path.empty()) continue;

        if (!file_filter.empty()) {
            auto base = std::filesystem::path(file_path).filename().string();
            bool matched = (file_path.find(file_filter) != std::string::npos) ||
                           (base.find(file_filter) != std::string::npos);
            if (!matched) continue;
        }

        auto symbols = rt_snap->get_file_enhanced_symbols(fid);
        for (const auto& sym : symbols) {
            if (!kind_matches(sym->symbol.type, kinds)) continue;
            if (exported_filter.has_value()) {
                if (*exported_filter != sym->is_exported) continue;
            }
            if (!name_filter.empty() &&
                !text::ascii_contains_ci(sym->symbol.name, name_filter)) {
                continue;
            }
            if (!receiver_filter.empty() &&
                text::ascii_lower(sym->receiver_type) != text::ascii_lower(receiver_filter)) {
                continue;
            }
            if (min_complexity.has_value() &&
                sym->complexity < *min_complexity) {
                continue;
            }
            if (max_complexity.has_value() &&
                sym->complexity > *max_complexity) {
                continue;
            }
            if (min_params.has_value() &&
                static_cast<int>(sym->parameter_count) < *min_params) {
                continue;
            }
            if (max_params.has_value() &&
                static_cast<int>(sym->parameter_count) > *max_params) {
                continue;
            }

            if (rows.empty() || row_paths.back() != file_path) {
                row_paths.push_back(file_path);
            }
            rows.push_back(
                {sym, static_cast<uint32_t>(row_paths.size() - 1)});
        }
    }

    // Server-side sort mirrors the CLI's sym_sort_symbols keys exactly
    // (complexity/refs/params descending, line/name ascending, empty =
    // deterministic file/line collection order). stable_sort over the
    // deterministic collection order keeps ties reproducible across runs.
    if (!sort_key.empty()) {
        auto by = [&](auto key_less) {
            std::stable_sort(rows.begin(), rows.end(), key_less);
        };
        if (sort_key == "complexity") {
            by([](const Row& a, const Row& b) {
                return a.sym->complexity > b.sym->complexity;
            });
        } else if (sort_key == "refs") {
            by([](const Row& a, const Row& b) {
                return a.sym->incoming_ref_count + a.sym->outgoing_ref_count >
                       b.sym->incoming_ref_count + b.sym->outgoing_ref_count;
            });
        } else if (sort_key == "params") {
            by([](const Row& a, const Row& b) {
                return a.sym->parameter_count > b.sym->parameter_count;
            });
        } else if (sort_key == "line") {
            by([&](const Row& a, const Row& b) {
                if (a.path_idx != b.path_idx) {
                    return row_paths[a.path_idx] < row_paths[b.path_idx];
                }
                return a.sym->symbol.line < b.sym->symbol.line;
            });
        } else {
            // Default + unknown -> name (ascending), like the CLI.
            by([](const Row& a, const Row& b) {
                return a.sym->symbol.name < b.sym->symbol.name;
            });
        }
    }

    const int total = static_cast<int>(rows.size());
    nlohmann::json entries = nlohmann::json::array();
    for (int i = offset;
         i < total && static_cast<int>(entries.size()) < max_results; ++i) {
        const auto& sym = rows[static_cast<size_t>(i)].sym;
        const std::string& file_path =
            row_paths[rows[static_cast<size_t>(i)].path_idx];

        // Mirror Go's `json:",omitempty"` semantics: only emit
        // numeric/string fields when non-zero / non-empty so that
        // canonicalised JSON matches the Go reference output.
        // Note: Go's /list-symbols handler intentionally omits the
        // `signature` field (it's exposed only via /inspect-symbol
        // in the Go reference). Match that here so summary listings
        // stay identical and signatures only appear where Go
        // surfaces them.
        nlohmann::json e;
        e["name"] = sym->symbol.name;
        e["type"] = std::string(to_string(sym->symbol.type));
        e["file"] = file_path;
        e["line"] = sym->symbol.line;
        e["object_id"] = encode_symbol_id(sym->id);
        e["is_exported"] = sym->is_exported;
        if (sym->complexity > 0) e["complexity"] = sym->complexity;
        if (sym->parameter_count > 0) {
            e["parameter_count"] = static_cast<int>(sym->parameter_count);
        }
        if (!sym->receiver_type.empty()) {
            e["receiver_type"] = sym->receiver_type;
        }
        if (sym->incoming_ref_count != 0) {
            e["incoming_refs"] = static_cast<int>(sym->incoming_ref_count);
        }
        if (sym->outgoing_ref_count != 0) {
            e["outgoing_refs"] = static_cast<int>(sym->outgoing_ref_count);
        }
        entries.push_back(e);
    }

    nlohmann::json j;
    j["symbols"] = entries;
    j["total"] = total;
    j["showing"] = static_cast<int>(entries.size());
    j["has_more"] =
        page_has_more(total, offset, static_cast<int>(entries.size()));
    json_response(res, j);
}

// -- Endpoint: /inspect-symbol ------------------------------------------------

void IndexServer::handle_inspect_symbol(const httplib::Request& req,
                                         httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    std::vector<ReferenceTracker::Snapshot::SymbolHandle> matched;

    // Pin the RCU snapshot for the lifetime of every pointer pulled from the
    // tracker below (matched[] elements and the type-hierarchy lookups all
    // point into this snapshot; it must outlive the serialization loop).
    auto rt_snap = indexer_->ref_tracker().pin();

    // Try by ID first
    auto id_str = body.value("id", "");
    if (!id_str.empty()) {
        auto decoded = decode_symbol_id(id_str);
        if (decoded.has_value()) {
            auto sym =
                rt_snap->get_enhanced_symbol(decoded.value());
            if (sym != nullptr) {
                matched.push_back(sym);
            }
        }
    }

    // Try by name if no ID match
    auto name_str = body.value("name", "");
    if (matched.empty() && !name_str.empty()) {
        matched = rt_snap->find_symbols_by_name(name_str);
    }

    // Apply disambiguators (file, type)
    auto file_filter = body.value("file", "");
    auto type_filter = body.value("type", "");
    if (!file_filter.empty() || !type_filter.empty()) {
        auto type_kinds = parse_symbol_kinds(type_filter);

        std::vector<ReferenceTracker::Snapshot::SymbolHandle> filtered;
        for (const auto& sym : matched) {
            if (!file_filter.empty()) {
                auto fp = indexer_->get_file_path(sym->symbol.file_id);
                auto base = std::filesystem::path(fp).filename().string();
                if (fp.find(file_filter) == std::string::npos &&
                    base.find(file_filter) == std::string::npos) {
                    continue;
                }
            }
            if (!type_kinds.empty() &&
                !kind_matches(sym->symbol.type, type_kinds)) {
                continue;
            }
            filtered.push_back(sym);
        }
        matched = filtered;
    }

    auto include_raw = body.value("include", "");
    const bool include_signature =
        include_raw == "all" || include_raw == "signature" ||
        include_raw.find("signature") != std::string::npos;

    auto& tracker = indexer_->ref_tracker();

    nlohmann::json symbols = nlohmann::json::array();
    for (const auto& sym : matched) {
        auto fp = indexer_->get_file_path(sym->symbol.file_id);

        nlohmann::json e;
        e["name"] = sym->symbol.name;
        e["object_id"] = encode_symbol_id(sym->id);
        e["type"] = std::string(to_string(sym->symbol.type));
        e["file"] = fp;
        e["line"] = sym->symbol.line;
        // Symbol bounds: emit `end_line` (and derived `lines_of_code`) when the
        // extractor populated it. Consumed by the CLI's `--enhanced` /
        // `--assembly` modes to render the surrounding block. Omitted with
        // `>` parity to git-analyze (which uses the same gating).
        if (sym->symbol.end_line > sym->symbol.line) {
            e["end_line"] = sym->symbol.end_line;
            e["lines_of_code"] =
                sym->symbol.end_line - sym->symbol.line + 1;
        }
        e["is_exported"] = sym->is_exported;
        e["complexity"] = sym->complexity;
        e["outgoing_refs"] = static_cast<int>(sym->outgoing_ref_count);
        if (include_signature && !sym->signature.empty()) {
            e["signature"] = sym->signature;
        }
        if (!sym->doc_comment.empty()) {
            e["doc_comment"] = sym->doc_comment;
        }
        if (sym->parameter_count > 0) {
            e["parameter_count"] = static_cast<int>(sym->parameter_count);
        }
        if (!sym->receiver_type.empty()) {
            e["receiver_type"] = sym->receiver_type;
        }
        if (sym->incoming_ref_count != 0) {
            e["incoming_refs"] = static_cast<int>(sym->incoming_ref_count);
        }

        // Callers/callees
        auto callers = tracker.get_caller_names(sym->id);
        if (!callers.empty()) {
            e["callers"] = callers;
        }
        auto callees = tracker.get_callee_names(sym->id);
        if (!callees.empty()) {
            e["callees"] = callees;
        }

        // Type hierarchy
        auto rels = tracker.get_type_relationships(sym->id);
        if (rels.has_relationships()) {
            nlohmann::json th;
            th["implements"] = nlohmann::json::array();
            th["implemented_by"] = nlohmann::json::array();
            th["extends"] = nlohmann::json::array();
            th["extended_by"] = nlohmann::json::array();

            for (auto id : rels.implements) {
                if (auto s = rt_snap->get_enhanced_symbol(id)) {
                    th["implements"].push_back(s->symbol.name);
                }
            }
            for (auto id : rels.implemented_by) {
                if (auto s = rt_snap->get_enhanced_symbol(id)) {
                    th["implemented_by"].push_back(s->symbol.name);
                }
            }
            for (auto id : rels.extends) {
                if (auto s = rt_snap->get_enhanced_symbol(id)) {
                    th["extends"].push_back(s->symbol.name);
                }
            }
            for (auto id : rels.extended_by) {
                if (auto s = rt_snap->get_enhanced_symbol(id)) {
                    th["extended_by"].push_back(s->symbol.name);
                }
            }
            e["type_hierarchy"] = th;
        }

        // Scope chain
        if (!sym->scope_chain.empty()) {
            nlohmann::json chain = nlohmann::json::array();
            for (const auto& sc : sym->scope_chain) {
                chain.push_back(sc.name);
            }
            e["scope_chain"] = chain;
        }

        // Function flags
        if (sym->function_flags != 0) {
            nlohmann::json flags = nlohmann::json::array();
            if (sym->is_async_func()) flags.push_back("async");
            if (sym->is_generator_func()) flags.push_back("generator");
            if (sym->is_method_func()) flags.push_back("method");
            if (sym->is_variadic_func()) flags.push_back("variadic");
            e["function_flags"] = flags;
        }

        // Variable flags
        if (sym->variable_flags != 0) {
            nlohmann::json flags = nlohmann::json::array();
            if (sym->is_const()) flags.push_back("const");
            if (sym->is_static()) flags.push_back("static");
            if (sym->is_pointer()) flags.push_back("pointer");
            e["variable_flags"] = flags;
        }

        symbols.push_back(e);
    }

    nlohmann::json j;
    j["symbols"] = symbols;
    j["count"] = static_cast<int>(symbols.size());
    json_response(res, j);
}

// -- Endpoint: /browse-file ---------------------------------------------------

void IndexServer::handle_browse_file(const httplib::Request& req,
                                      httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    FileID target_fid = 0;
    std::string target_path;
    bool found = false;

    // Try by file_id first
    if (body.contains("file_id") && !body["file_id"].is_null()) {
        target_fid = static_cast<FileID>(body["file_id"].get<int>());
        target_path = indexer_->get_file_path(target_fid);
        if (!target_path.empty()) found = true;
    }

    // Try by file path
    auto file_str = body.value("file", "");
    if (!found && !file_str.empty()) {
        auto all_ids = indexer_->get_all_file_ids();
        for (auto fid : all_ids) {
            auto fp = indexer_->get_file_path(fid);
            if (fp.empty()) continue;

            bool match = (fp == file_str);
            if (!match) {
                // Check suffix match
                if (fp.size() > file_str.size()) {
                    auto sep = fp[fp.size() - file_str.size() - 1];
                    match = (sep == '/' || sep == '\\') &&
                            fp.substr(fp.size() - file_str.size()) == file_str;
                }
            }
            if (match) {
                target_fid = fid;
                target_path = fp;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        nlohmann::json j;
        j["error"] = "file not found: " + file_str;
        json_response(res, j);
        return;
    }

    auto kind_str = body.value("kind", "");
    auto kinds = parse_symbol_kinds(kind_str);
    std::optional<bool> exported_filter;
    if (body.contains("exported") && !body["exported"].is_null()) {
        exported_filter = body["exported"].get<bool>();
    }
    int max_results = body.value("max", 100);
    if (max_results <= 0) max_results = 100;

    auto rt_snap = indexer_->ref_tracker().pin();
    auto symbols = rt_snap->get_file_enhanced_symbols(target_fid);

    nlohmann::json entries = nlohmann::json::array();
    int total = 0;
    for (const auto& sym : symbols) {
        if (!kind_matches(sym->symbol.type, kinds)) continue;
        if (exported_filter.has_value() &&
            *exported_filter != sym->is_exported) {
            continue;
        }
        ++total;
        if (static_cast<int>(entries.size()) >= max_results) continue;

        // Same omitempty treatment as /list-symbols so HTTP browse-file
        // shape matches Go's reference encoder field-for-field. Go's
        // /browse-file (like /list-symbols) intentionally omits the
        // `signature` field, surfacing it only through /inspect-symbol.
        nlohmann::json e;
        e["name"] = sym->symbol.name;
        e["type"] = std::string(to_string(sym->symbol.type));
        e["file"] = target_path;
        e["line"] = sym->symbol.line;
        // Same end_line/lines_of_code emission as /list-symbols so the CLI's
        // enhanced/assembly output modes can resolve enclosing-block bounds
        // via either entry point. Gated on `end_line > line` to avoid
        // poisoning consumers with unset zero-bounds rows.
        if (sym->symbol.end_line > sym->symbol.line) {
            e["end_line"] = sym->symbol.end_line;
            e["lines_of_code"] =
                sym->symbol.end_line - sym->symbol.line + 1;
        }
        e["object_id"] = encode_symbol_id(sym->id);
        e["is_exported"] = sym->is_exported;
        if (sym->complexity > 0) e["complexity"] = sym->complexity;
        if (sym->parameter_count > 0) {
            e["parameter_count"] = static_cast<int>(sym->parameter_count);
        }
        if (!sym->receiver_type.empty()) {
            e["receiver_type"] = sym->receiver_type;
        }
        if (sym->incoming_ref_count != 0) {
            e["incoming_refs"] = static_cast<int>(sym->incoming_ref_count);
        }
        if (sym->outgoing_ref_count != 0) {
            e["outgoing_refs"] = static_cast<int>(sym->outgoing_ref_count);
        }
        entries.push_back(e);
    }

    nlohmann::json j;
    j["file"]["path"] = target_path;
    j["file"]["file_id"] = static_cast<int>(target_fid);
    j["file"]["language"] = language_from_path(target_path);
    j["symbols"] = entries;
    j["total"] = total;

    // Optional imports
    if (body.value("show_imports", false)) {
        auto fc = indexer_->file_content_store().get_file(target_fid);
        // Imports are stored on FileInfo which isn't directly accessible
        // from the current C++ API. Return empty for now.
        j["imports"] = nlohmann::json::array();
    }

    // Optional stats
    if (body.value("show_stats", false)) {
        int func_count = 0;
        int type_count = 0;
        int exported_count = 0;
        int max_cx = 0;
        int total_cx = 0;
        int cx_count = 0;

        for (const auto& sym : symbols) {
            if (sym->is_exported) ++exported_count;
            if (sym->symbol.type == SymbolType::Function ||
                sym->symbol.type == SymbolType::Method) {
                ++func_count;
                if (sym->complexity > 0) {
                    total_cx += sym->complexity;
                    ++cx_count;
                    if (sym->complexity > max_cx) max_cx = sym->complexity;
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
        stats["avg_complexity"] =
            cx_count > 0 ? static_cast<double>(total_cx) / cx_count : 0.0;
        stats["max_complexity"] = max_cx;
        stats["exported_count"] = exported_count;
        j["stats"] = stats;
    }

    json_response(res, j);
}

}  // namespace lci
