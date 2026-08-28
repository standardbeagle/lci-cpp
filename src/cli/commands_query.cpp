#include <lci/cli/commands.h>

#include "name_aggregation.h"
#include "commands_shared.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>

#include <lci/indexing/pipeline_scanner.h>
#include <lci/semantic/fuzzy_matcher.h>
#include <nlohmann/json.hpp>

#include "ast_filters.h"
#include "symbol_filters.h"
#include "tree_formatter.h"

namespace lci {
namespace cli {

namespace fs = std::filesystem;

// -- git-analyze command ------------------------------------------------------

int run_git_analyze(const GlobalFlags& flags, const std::string& scope,
                    const std::string& base_ref, const std::string& target_ref,
                    const std::vector<std::string>& focus, double threshold,
                    int max_findings, bool json_output) {
    if (scope != "staged" && scope != "wip" && scope != "commit" &&
        scope != "range") {
        std::cerr << "Error: invalid scope: " << scope
                  << " (must be staged, wip, commit, or range)\n";
        return 1;
    }

    if (scope == "range" && base_ref.empty()) {
        std::cerr << "Error: --base is required for range scope\n";
        return 1;
    }

    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::string conn_err;
    auto client = ensure_server_running(cfg, conn_err);
    if (!client) {
        std::cerr << "Error: " << conn_err << "\n";
        return 1;
    }

    // Change analysis compares changed symbols against the whole index and
    // can far exceed the default 30s read timeout on large repos; the
    // request is legitimate long-work, so give it a long leash instead of
    // failing with a misleading "failed to connect".
    client->set_timeout(std::chrono::minutes(5));

    GitAnalyzeRequest req;
    req.scope = scope;
    req.base_ref = base_ref;
    req.target_ref = target_ref;
    req.focus = focus;
    req.similarity_threshold = threshold;
    req.max_findings = max_findings;

    std::string analyze_err;
    auto result = client->git_analyze(req, analyze_err);
    if (!result) {
        std::cerr << "Error: analysis failed: " << analyze_err << "\n";
        return 1;
    }

    const auto& report =
        (result->contains("report") && (*result)["report"].is_object())
            ? (*result)["report"]
            : *result;

    if (json_output) {
        std::cout << report.dump(2) << "\n";
        return 0;
    }

    // Text output matching Go implementation
    std::printf("Git Change Analysis\n");
    std::printf("==================\n\n");

    // The default scope reads the git INDEX only, so a dirty tree with
    // nothing staged reports "Files changed: 0" — which reads as "no work
    // in progress" and sent users away thinking the tool was broken. Name
    // the scope boundary whenever the staged scope comes back empty.
    if (scope == "staged" && report.contains("summary") &&
        report["summary"].value("files_changed", 0) == 0) {
        std::printf(
            "note: scope 'staged' analyzes staged (git index) changes only;"
            " use `-s wip` for uncommitted working-tree changes\n\n");
    }

    if (report.contains("summary")) {
        auto& summary = report["summary"];
        std::printf("Summary\n");
        std::printf("-------\n");
        std::printf("Files changed: %d | Symbols: +%d ~%d\n",
                    summary.value("files_changed", 0),
                    summary.value("symbols_added", 0),
                    summary.value("symbols_modified", 0));
        std::printf("Issues: %d duplicates, %d naming | Risk: %.0f%%\n",
                    summary.value("duplicates_found", 0),
                    summary.value("naming_issues_found", 0),
                    summary.value("risk_score", 0.0) * 100.0);

        if (summary.contains("top_recommendation") &&
            !summary["top_recommendation"].get<std::string>().empty()) {
            std::printf("\nTop recommendation: %s\n",
                        summary["top_recommendation"]
                            .get<std::string>()
                            .c_str());
        }
    }

    if (report.contains("duplicates") && report["duplicates"].is_array()) {
        auto& dups = report["duplicates"];
        if (!dups.empty()) {
            std::printf("\nDuplicates\n");
            std::printf("----------\n");
            for (const auto& dup : dups) {
                std::string severity = dup.value("severity", "");
                for (auto& c : severity) c = static_cast<char>(std::toupper(c));
                std::printf("[%s] %s duplicate (%.0f%%)\n", severity.c_str(),
                            dup.value("type", "").c_str(),
                            dup.value("similarity", 0.0) * 100.0);
                if (dup.contains("new_code")) {
                    std::printf("  New: %s:%d (%s)\n",
                                dup["new_code"].value("file_path", "").c_str(),
                                dup["new_code"].value("start_line", 0),
                                dup["new_code"]
                                    .value("symbol_name", "")
                                    .c_str());
                }
                if (dup.contains("existing_code")) {
                    std::printf(
                        "  Existing: %s:%d (%s)\n",
                        dup["existing_code"].value("file_path", "").c_str(),
                        dup["existing_code"].value("start_line", 0),
                        dup["existing_code"]
                            .value("symbol_name", "")
                            .c_str());
                }
                std::printf("  -> %s\n",
                            dup.value("suggestion", "").c_str());
            }
        }
    }

    if (report.contains("naming_issues") &&
        report["naming_issues"].is_array()) {
        auto& issues = report["naming_issues"];
        if (!issues.empty()) {
            std::printf("\nNaming Issues\n");
            std::printf("-------------\n");
            for (const auto& issue : issues) {
                std::string severity = issue.value("severity", "");
                for (auto& c : severity)
                    c = static_cast<char>(std::toupper(c));
                std::printf("[%s] %s\n", severity.c_str(),
                            issue.value("issue_type", "").c_str());
                if (issue.contains("new_symbol")) {
                    std::printf(
                        "  Symbol: %s (%s:%d)\n",
                        issue["new_symbol"].value("name", "").c_str(),
                        issue["new_symbol"].value("file_path", "").c_str(),
                        issue["new_symbol"].value("line", 0));
                }
                std::printf("  Issue: %s\n",
                            issue.value("issue", "").c_str());
                std::printf("  -> %s\n",
                            issue.value("suggestion", "").c_str());
            }
        }
    }

    if (report.contains("metadata")) {
        auto& meta = report["metadata"];
        std::printf("\nAnalysis: %s -> %s (%dms)\n",
                    meta.value("base_ref", "").c_str(),
                    meta.value("target_ref", "").c_str(),
                    meta.value("analysis_time_ms", 0));
    }

    return 0;
}

// -- symbols command ----------------------------------------------------------

namespace {

// Returns true if `s` contains glob metacharacters. A bare substring
// returns false and is intended to be passed to the server's substring
// file filter unchanged.
bool sym_is_glob_pattern(std::string_view s) {
    for (char c : s) {
        if (c == '*' || c == '?' || c == '[') return true;
    }
    return false;
}

// Single-segment glob matcher (Go `filepath.Match` semantics for the
// simple forms `lci symbols --file` accepts). `*` matches any run of
// non-`/`, `?` matches any single non-`/`. Iterative implementation with
// star-backtracking; no recursion.
bool sym_glob_match(std::string_view pattern, std::string_view text) {
    size_t px = 0, tx = 0;
    size_t star_px = std::string_view::npos;
    size_t star_tx = 0;

    while (tx < text.size()) {
        if (px < pattern.size() && pattern[px] == '*') {
            // Single * — does not cross '/'.
            star_px = px + 1;
            star_tx = tx;
            ++px;
            continue;
        }
        if (px < pattern.size() && pattern[px] == '?') {
            if (text[tx] != '/') {
                ++px;
                ++tx;
                continue;
            }
        } else if (px < pattern.size() && pattern[px] == text[tx]) {
            ++px;
            ++tx;
            continue;
        }
        // Backtrack to last star (but only if neither side has crossed '/').
        if (star_px != std::string_view::npos && text[star_tx] != '/') {
            px = star_px;
            ++star_tx;
            tx = star_tx;
            continue;
        }
        return false;
    }
    while (px < pattern.size() && pattern[px] == '*') ++px;
    return px == pattern.size();
}

bool sym_glob_match_path_or_basename(std::string_view pattern,
                                     std::string_view path) {
    if (sym_glob_match(pattern, path)) return true;
    auto slash = path.find_last_of('/');
    if (slash == std::string_view::npos) return false;
    return sym_glob_match(pattern, path.substr(slash + 1));
}

nlohmann::json sym_apply_file_glob(nlohmann::json symbols,
                                   std::string_view pattern) {
    if (pattern.empty()) return symbols;
    if (!symbols.is_array()) return symbols;
    nlohmann::json out = nlohmann::json::array();
    for (auto& s : symbols) {
        std::string fpath = s.value("file", "");
        if (sym_glob_match_path_or_basename(pattern, fpath)) {
            out.push_back(std::move(s));
        }
    }
    return out;
}

// Sort key extractors — each returns a comparable tuple. Stable sort over
// the input array, secondary key is original index (preserved by
// std::stable_sort).
nlohmann::json sym_sort_symbols(nlohmann::json symbols,
                                std::string_view sort_key) {
    if (sort_key.empty()) return symbols;
    if (!symbols.is_array()) return symbols;

    auto vec = symbols.get<std::vector<nlohmann::json>>();

    if (sort_key == "complexity") {
        std::stable_sort(vec.begin(), vec.end(),
                         [](const nlohmann::json& a, const nlohmann::json& b) {
                             return a.value("complexity", 0) >
                                    b.value("complexity", 0);
                         });
    } else if (sort_key == "refs") {
        std::stable_sort(
            vec.begin(), vec.end(),
            [](const nlohmann::json& a, const nlohmann::json& b) {
                int ra = a.value("incoming_refs", 0) +
                         a.value("outgoing_refs", 0);
                int rb = b.value("incoming_refs", 0) +
                         b.value("outgoing_refs", 0);
                return ra > rb;
            });
    } else if (sort_key == "line") {
        std::stable_sort(
            vec.begin(), vec.end(),
            [](const nlohmann::json& a, const nlohmann::json& b) {
                std::string fa = a.value("file", "");
                std::string fb = b.value("file", "");
                if (fa != fb) return fa < fb;
                return a.value("line", 0) < b.value("line", 0);
            });
    } else if (sort_key == "params") {
        std::stable_sort(vec.begin(), vec.end(),
                         [](const nlohmann::json& a, const nlohmann::json& b) {
                             return a.value("parameter_count", 0) >
                                    b.value("parameter_count", 0);
                         });
    } else {
        // Default + unknown -> name (ascending).
        std::stable_sort(vec.begin(), vec.end(),
                         [](const nlohmann::json& a, const nlohmann::json& b) {
                             return a.value("name", "") < b.value("name", "");
                         });
    }

    nlohmann::json out = nlohmann::json::array();
    for (auto& s : vec) out.push_back(std::move(s));
    return out;
}

nlohmann::json sym_apply_max_limit(nlohmann::json symbols, int max_results) {
    if (max_results <= 0) return symbols;
    if (!symbols.is_array()) return symbols;
    if (static_cast<int>(symbols.size()) <= max_results) return symbols;
    nlohmann::json out = nlohmann::json::array();
    for (int i = 0; i < max_results; ++i) {
        out.push_back(std::move(symbols[i]));
    }
    return out;
}

}  // namespace

namespace symbol_filters {

bool is_glob_pattern(std::string_view s) { return sym_is_glob_pattern(s); }

bool glob_match(std::string_view pattern, std::string_view text) {
    return sym_glob_match(pattern, text);
}

bool glob_match_path_or_basename(std::string_view pattern,
                                 std::string_view path) {
    return sym_glob_match_path_or_basename(pattern, path);
}

nlohmann::json apply_file_glob(nlohmann::json symbols,
                               std::string_view pattern) {
    return sym_apply_file_glob(std::move(symbols), pattern);
}

nlohmann::json sort_symbols(nlohmann::json symbols,
                            std::string_view sort_key) {
    return sym_sort_symbols(std::move(symbols), sort_key);
}

nlohmann::json apply_max_limit(nlohmann::json symbols, int max_results) {
    return sym_apply_max_limit(std::move(symbols), max_results);
}

}  // namespace symbol_filters

int run_symbols(const GlobalFlags& flags, const std::string& kind,
                bool exported, const std::string& file,
                const std::string& name, const std::string& receiver,
                int min_complexity, int max_complexity,
                const std::string& sort, int max_results, bool json_output) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::string conn_err;
    auto client = ensure_server_running(cfg, conn_err);
    if (!client) {
        std::cerr << "Error: " << conn_err << "\n";
        return 1;
    }

    // Decide whether the --file value is a glob (`*`, `?`, `[`) or a plain
    // substring. The C++ server only does substring matching on `file`, so
    // a glob like `*.cpp` would drop almost everything if forwarded. We keep
    // server-side filtering for non-glob inputs (faster) and post-process
    // glob inputs client-side.
    const bool file_is_glob = sym_is_glob_pattern(file);

    ListSymbolsRequest req;
    req.kind = kind;
    if (!file_is_glob) {
        req.file = file;
    }
    req.name = name;
    req.receiver = receiver;
    req.sort = sort;
    // Always pull the server's max page (server caps at 500). We apply the
    // user's --max client-side after sort/glob so post-processing sees the
    // full candidate set, not a server-truncated head.
    req.max = 500;
    if (exported) {
        req.exported = true;
    }
    if (min_complexity > 0) {
        req.min_complexity = min_complexity;
    }
    if (max_complexity > 0) {
        req.max_complexity = max_complexity;
    }

    std::string sym_err;
    auto result = client->list_symbols(req, sym_err);
    if (!result) {
        std::cerr << "Error: list symbols failed: " << sym_err << "\n";
        return 1;
    }

    // Client-side post-processing: glob file filter, sort, then --max.
    nlohmann::json symbols_arr = nlohmann::json::array();
    if (result->contains("symbols") && (*result)["symbols"].is_array()) {
        symbols_arr = (*result)["symbols"];
    }
    if (file_is_glob) {
        symbols_arr = sym_apply_file_glob(std::move(symbols_arr), file);
    }
    symbols_arr = sym_sort_symbols(std::move(symbols_arr), sort);

    int total_after_filter = static_cast<int>(symbols_arr.size());
    int effective_max = max_results > 0 ? max_results : 50;
    symbols_arr = sym_apply_max_limit(std::move(symbols_arr), effective_max);

    // Update the response envelope so JSON consumers see the post-processed
    // counts and ordering. `total` reflects the count after client-side
    // filtering (including glob); `showing` is what we're emitting now;
    // `has_more` is true iff the user's --max truncated the set.
    int showing = static_cast<int>(symbols_arr.size());
    (*result)["symbols"] = symbols_arr;
    (*result)["total"] = total_after_filter;
    (*result)["showing"] = showing;
    (*result)["has_more"] = showing < total_after_filter;

    if (json_output) {
        std::cout << result->dump(2) << "\n";
        return 0;
    }

    // Text output matching Go implementation
    for (const auto& sym : symbols_arr) {
        std::string sig = sym.value("signature", "");
        if (sig.empty()) {
            sig = sym.value("name", "");
        }
        std::string exp_str;
        if (sym.value("is_exported", false)) {
            exp_str = " [exported]";
        }
        std::string comp_str;
        int comp = sym.value("complexity", 0);
        if (comp > 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), " (complexity:%d)", comp);
            comp_str = buf;
        }
        std::printf("%s:%d: %s %s%s%s\n", sym.value("file", "").c_str(),
                    sym.value("line", 0), sym.value("type", "").c_str(),
                    sig.c_str(), exp_str.c_str(), comp_str.c_str());
    }

    if (showing < total_after_filter) {
        std::fprintf(stderr, "\n(%d of %d shown, use --max to see more)\n",
                     showing, total_after_filter);
    }

    return 0;
}

// -- inspect command ----------------------------------------------------------

namespace {

// JSON output keeps the full list; text mode aggregates via
// cli::format_aggregated_names.
void print_aggregated_names(const nlohmann::json& arr, const char* label) {
    std::vector<std::string> names;
    names.reserve(arr.size());
    for (const auto& c : arr) names.push_back(c.get<std::string>());
    std::printf("  %s: %s\n", label,
                format_aggregated_names(names).c_str());
}

}  // namespace

int run_inspect(const GlobalFlags& flags, const std::string& name,
                const std::string& type, const std::string& file,
                const std::string& include_sections, bool json_output) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::string conn_err;
    auto client = ensure_server_running(cfg, conn_err);
    if (!client) {
        std::cerr << "Error: " << conn_err << "\n";
        return 1;
    }

    InspectSymbolRequest req;
    req.name = name;
    req.type = type;
    req.file = file;
    req.include = include_sections.empty() ? "signature" : include_sections;

    std::string insp_err;
    auto result = client->inspect_symbol(req, insp_err);
    if (!result) {
        std::cerr << "Error: inspect failed: " << insp_err << "\n";
        return 1;
    }

    if (json_output) {
        // JSON shape on a miss is spec-pinned (inspect-missing-json:
        // count 0, symbols [], exit 0) — no diagnosis lines here.
        std::cout << result->dump(2) << "\n";
        return 0;
    }

    // Text output matching Go implementation
    if (!result->contains("symbols") || !(*result)["symbols"].is_array() ||
        (*result)["symbols"].empty()) {
        // A miss used to print NOTHING and exit 0 — indistinguishable from
        // a crash or an empty pipe. Diagnose like `lci def`: name the miss,
        // offer near-miss candidates, keep exit 0 ("no result" answers a
        // valid query; nonzero stays reserved for real failures).
        std::printf("no symbol in index named %s\n", name.c_str());
        print_symbol_suggestions(*client, name);
        return 0;
    }
    if (result->contains("symbols") && (*result)["symbols"].is_array()) {
        int idx = 0;
        for (const auto& sym : (*result)["symbols"]) {
            if (idx > 0) {
                std::printf("---\n");
            }
            std::printf("%s (%s) %s:%d\n", sym.value("name", "").c_str(),
                        sym.value("type", "").c_str(),
                        sym.value("file", "").c_str(),
                        sym.value("line", 0));

            std::string sig = sym.value("signature", "");
            if (!sig.empty()) {
                std::printf("  Signature: %s\n", sig.c_str());
            }
            std::string doc = sym.value("doc_comment", "");
            if (!doc.empty()) {
                std::printf("  Doc: %s\n", doc.c_str());
            }
            int comp = sym.value("complexity", 0);
            if (comp > 0) {
                std::printf("  Complexity: %d\n", comp);
            }
            std::string recv = sym.value("receiver_type", "");
            if (!recv.empty()) {
                std::printf("  Receiver: %s\n", recv.c_str());
            }
            if (sym.contains("callers") && sym["callers"].is_array() &&
                !sym["callers"].empty()) {
                print_aggregated_names(sym["callers"], "Callers");
            }
            if (sym.contains("callees") && sym["callees"].is_array() &&
                !sym["callees"].empty()) {
                print_aggregated_names(sym["callees"], "Callees");
            }
            if (sym.contains("type_hierarchy") &&
                !sym["type_hierarchy"].is_null()) {
                auto& th = sym["type_hierarchy"];
                if (th.contains("implements") && !th["implements"].empty()) {
                    std::printf("  Implements: ");
                    bool first = true;
                    for (const auto& v : th["implements"]) {
                        if (!first) std::printf(", ");
                        std::printf("%s", v.get<std::string>().c_str());
                        first = false;
                    }
                    std::printf("\n");
                }
                if (th.contains("implemented_by") &&
                    !th["implemented_by"].empty()) {
                    std::printf("  Implemented by: ");
                    bool first = true;
                    for (const auto& v : th["implemented_by"]) {
                        if (!first) std::printf(", ");
                        std::printf("%s", v.get<std::string>().c_str());
                        first = false;
                    }
                    std::printf("\n");
                }
            }
            if (sym.contains("scope_chain") && sym["scope_chain"].is_array() &&
                !sym["scope_chain"].empty()) {
                std::printf("  Scope: ");
                bool first = true;
                for (const auto& s : sym["scope_chain"]) {
                    if (!first) std::printf(" > ");
                    std::printf("%s", s.get<std::string>().c_str());
                    first = false;
                }
                std::printf("\n");
            }
            if (sym.contains("annotations") &&
                sym["annotations"].is_array() &&
                !sym["annotations"].empty()) {
                std::printf("  Annotations: ");
                bool first = true;
                for (const auto& a : sym["annotations"]) {
                    if (!first) std::printf(", ");
                    std::printf("%s", a.get<std::string>().c_str());
                    first = false;
                }
                std::printf("\n");
            }
            std::printf("  Refs: %d incoming, %d outgoing\n",
                        sym.value("incoming_refs", 0),
                        sym.value("outgoing_refs", 0));
            ++idx;
        }
    }

    return 0;
}

// -- browse command -----------------------------------------------------------

int run_browse(const GlobalFlags& flags, const std::string& file_path,
               const std::string& kind, bool exported,
               const std::string& sort, bool show_imports, bool show_stats,
               bool json_output) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::string conn_err;
    auto client = ensure_server_running(cfg, conn_err);
    if (!client) {
        std::cerr << "Error: " << conn_err << "\n";
        return 1;
    }

    BrowseFileRequest req;
    req.file = file_path;
    req.kind = kind;
    req.sort = sort;
    req.show_imports = show_imports;
    // Go's browse surface accepts --stats but does not currently enrich the
    // CLI/JSON payload with a dedicated stats block for this command path.
    // Keep C++ aligned with that contract instead of emitting extra fields.
    req.show_stats = false;
    if (exported) {
        req.exported = true;
    }

    std::string browse_err;
    auto result = client->browse_file(req, browse_err);
    if (!result) {
        std::cerr << "Error: browse failed: " << browse_err << "\n";
        return 1;
    }

    if (json_output) {
        std::cout << result->dump(2) << "\n";
        return 0;
    }

    // Text output matching Go implementation
    if (result->contains("file")) {
        auto& fi = (*result)["file"];
        std::printf("File: %s", fi.value("path", "").c_str());
        std::string lang = fi.value("language", "");
        if (!lang.empty()) {
            std::printf(" (%s)", lang.c_str());
        }
        std::printf("\n");
    }

    if (result->contains("stats") && !(*result)["stats"].is_null()) {
        auto& st = (*result)["stats"];
        std::printf("Stats: %d symbols (%d functions, %d types, %d exported)",
                    st.value("symbol_count", 0),
                    st.value("function_count", 0), st.value("type_count", 0),
                    st.value("exported_count", 0));
        double avg_comp = st.value("avg_complexity", 0.0);
        if (avg_comp > 0) {
            std::printf(", avg complexity: %.1f, max: %d", avg_comp,
                        st.value("max_complexity", 0));
        }
        std::printf("\n");
    }

    if (result->contains("imports") && (*result)["imports"].is_array() &&
        !(*result)["imports"].empty()) {
        std::printf("\nImports:\n");
        for (const auto& imp : (*result)["imports"]) {
            std::printf("  %s\n", imp.get<std::string>().c_str());
        }
    }

    if (result->contains("symbols") && (*result)["symbols"].is_array()) {
        int total = result->value("total", 0);
        std::printf("\nSymbols (%d):\n", total);
        for (const auto& sym : (*result)["symbols"]) {
            std::string sig = sym.value("signature", "");
            if (sig.empty()) {
                sig = sym.value("name", "");
            }
            std::string exp_str;
            if (sym.value("is_exported", false)) {
                exp_str = " [exported]";
            }
            std::printf("  %4d: %-10s %s%s\n", sym.value("line", 0),
                        sym.value("type", "").c_str(), sig.c_str(),
                        exp_str.c_str());
        }
    }

    return 0;
}

}  // namespace cli
}  // namespace lci
