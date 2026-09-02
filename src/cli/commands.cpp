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

// -- def zero-result diagnosis helpers ----------------------------------------

namespace {

// Left-trims ASCII spaces and tabs.
std::string_view ltrim_view(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

bool is_ident_char(unsigned char c) {
    return std::isalnum(c) != 0 || c == '_';
}

// True if `word` appears in `line` bounded by non-identifier characters, so
// "import" matches `from x import y` but not `important`.
bool contains_word(std::string_view line, std::string_view word) {
    if (word.empty()) return false;
    size_t pos = line.find(word);
    while (pos != std::string_view::npos) {
        const bool left_ok =
            pos == 0 ||
            !is_ident_char(static_cast<unsigned char>(line[pos - 1]));
        const size_t after = pos + word.size();
        const bool right_ok =
            after >= line.size() ||
            !is_ident_char(static_cast<unsigned char>(line[after]));
        if (left_ok && right_ok) return true;
        pos = line.find(word, pos + 1);
    }
    return false;
}

}  // namespace

bool line_imports_symbol(std::string_view line, std::string_view symbol) {
    if (symbol.empty()) return false;
    std::string_view t = ltrim_view(line);
    const bool is_import =
        t.starts_with("import ") ||
        (t.starts_with("from ") && contains_word(t, "import")) ||
        contains_word(t, "require");
    if (!is_import) return false;
    // Only an import *of this symbol* counts as its import site.
    return contains_word(line, symbol);
}

std::string import_module_of(std::string_view line) {
    std::string_view t = ltrim_view(line);
    if (!t.starts_with("from ")) return "";
    t.remove_prefix(std::string_view("from ").size());
    t = ltrim_view(t);
    size_t end = 0;
    while (end < t.size() &&
           (is_ident_char(static_cast<unsigned char>(t[end])) || t[end] == '.')) {
        ++end;
    }
    return std::string(t.substr(0, end));
}

// -- near-miss suggestions (shared by def / inspect misses) -------------------

namespace {

/// Folds a symbol name for near-miss comparison: ASCII-lowercase with
/// underscores dropped, so `page_window` and `PageWindow` compare equal and
/// jaro-winkler measures the remaining real difference. Display always uses
/// the original candidate.
std::string fold_symbol_name(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '_') continue;
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// Prints a `did you mean: ...?` line for `symbol` when the index holds
/// near-miss names; returns true when at least one suggestion was printed.
///
/// The candidate pool is TARGETED, not a truncated global page: the server's
/// /list-symbols name filter is a case-insensitive substring match, so one
/// query with the full symbol catches case-only mismatches outright, and one
/// query per >=3-char identifier token catches renames that keep a word
/// (`page_window` -> `PageWindow` via "page"/"window"). A general unfiltered
/// page tops the pool up for pure typos. The previous implementation ranked
/// only the server's arbitrary first-500 page of ~10k symbols, so most real
/// near-misses were never even considered.
}  // namespace

bool print_symbol_suggestions(Client& client, const std::string& symbol) {
    std::vector<std::string> candidates;
    std::set<std::string> seen_candidates;
    auto pool_query = [&](const std::string& name_filter, int max) {
        ListSymbolsRequest req;
        req.name = name_filter;
        req.max = max;
        std::string err;
        auto listed = client.list_symbols(req, err);
        if (!listed || !listed->contains("symbols") ||
            !(*listed)["symbols"].is_array()) {
            return;
        }
        for (const auto& s : (*listed)["symbols"]) {
            std::string name = s.value("name", "");
            if (!name.empty() && seen_candidates.insert(name).second) {
                candidates.push_back(std::move(name));
            }
        }
    };

    pool_query(symbol, 100);  // ci substring of the full query
    int token_queries = 0;
    size_t i = 0;
    while (i < symbol.size() && token_queries < 3) {
        while (i < symbol.size() &&
               !is_ident_char(static_cast<unsigned char>(symbol[i]))) {
            ++i;
        }
        size_t start = i;
        while (i < symbol.size() &&
               is_ident_char(static_cast<unsigned char>(symbol[i]))) {
            ++i;
        }
        // Skip the full-symbol duplicate; is_ident_char runs only split on
        // separators like `::` or `.`.
        std::string token = symbol.substr(start, i - start);
        if (token.size() >= 3 && token != symbol) {
            pool_query(token, 100);
            ++token_queries;
        }
    }
    // Underscore-delimited words of the query as extra token filters.
    {
        size_t start = 0;
        int extra = 0;
        for (size_t j = 0; j <= symbol.size() && extra < 3; ++j) {
            if (j == symbol.size() || symbol[j] == '_') {
                std::string word = symbol.substr(start, j - start);
                if (word.size() >= 3 && word != symbol) {
                    pool_query(word, 100);
                    ++extra;
                }
                start = j + 1;
            }
        }
    }
    pool_query("", 500);  // broad page for pure typos

    // Rank on FOLDED names (case/underscore-insensitive) with the project's
    // fuzzy default (jaro-winkler, 0.7 — see SemanticScoringConfig).
    FuzzyMatcher matcher(/*enabled=*/true, /*threshold=*/0.7, "jaro-winkler");
    const std::string folded_target = fold_symbol_name(symbol);
    struct Scored {
        const std::string* name;
        double similarity;
    };
    std::vector<Scored> scored;
    scored.reserve(candidates.size());
    for (const auto& c : candidates) {
        double sim = matcher.similarity(folded_target, fold_symbol_name(c));
        if (sim >= 0.7) scored.push_back({&c, sim});
    }
    if (scored.empty()) return false;
    std::stable_sort(scored.begin(), scored.end(),
                     [](const Scored& a, const Scored& b) {
                         return a.similarity > b.similarity;
                     });

    constexpr size_t kMaxSuggestions = 5;
    std::printf("did you mean: ");
    for (size_t j = 0; j < scored.size() && j < kMaxSuggestions; ++j) {
        std::printf("%s%s", j == 0 ? "" : ", ", scored[j].name->c_str());
    }
    std::printf("?\n");
    return true;
}

namespace {

}  // namespace

// -- def command --------------------------------------------------------------

int run_def(const GlobalFlags& flags, const std::string& symbol) {
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

    std::string def_err;
    auto results = client->get_definition(symbol, 100, def_err);
    if (!results) {
        std::cerr << "Error: definition search failed: " << def_err << "\n";
        return 1;
    }

    if (!results->empty()) {
        for (const auto& r : *results) {
            if (!r.signature.empty()) {
                // Real symbol kind in brackets + the verbatim declaration line,
                // e.g. `path:1455 [function] def check_random_state(seed)`. The
                // signature already names the symbol/keyword, so the kind
                // annotates it rather than repeating `type name`. Answers the
                // "what kind + what signature" question without a follow-up read.
                std::printf("%s:%d [%s] %s\n", r.file_path.c_str(), r.line,
                            r.type.c_str(), r.signature.c_str());
            } else {
                // No signature resolved (generic text hit, no indexed symbol at
                // this location): degrade to the prior `type name` form rather
                // than printing empty brackets.
                std::printf("%s:%d: %s %s\n", r.file_path.c_str(), r.line,
                            r.type.c_str(), r.name.c_str());
            }
        }
        return 0;
    }

    // Zero definitions in the index is ambiguous between an external-dependency
    // symbol (imported, defined outside the repo), an indexing gap, and a typo.
    // Emit an explicit diagnosis instead of exiting silently. Exit code stays 0:
    // "no result" is an informational answer to a valid query, not a hard error
    // — matching `lci search` on a non-existent pattern (tests/integration/cli/
    // search/no-results.spec.json expects exit 0). Reserve nonzero for genuine
    // failures (bad config, server/connection errors) already handled above.
    std::printf("no definition in index for %s\n", symbol.c_str());

    // (1) Is it imported? Reuse the reference search (text-based) and surface
    // any hit whose source line is an import statement for this symbol. This
    // explains "external dependency" without a wire-format change: the import
    // line (e.g. `from joblib import effective_n_jobs`) names the module.
    std::string refs_err;
    if (auto refs = client->get_references(symbol, 100, refs_err)) {
        std::set<std::string> seen;
        int shown = 0;
        constexpr int kMaxImportSites = 10;
        for (const auto& r : *refs) {
            if (shown >= kMaxImportSites) break;
            if (!line_imports_symbol(r.context, symbol)) continue;
            std::string key = r.file_path + ":" + std::to_string(r.line);
            if (!seen.insert(key).second) continue;
            if (std::string mod = import_module_of(r.context); !mod.empty()) {
                std::printf("  imported from %s at %s:%d\n", mod.c_str(),
                            r.file_path.c_str(), r.line);
            } else {
                // Plain `import x` / JS / require: print the site verbatim.
                std::string_view line = ltrim_view(r.context);
                std::printf("  imported at %s:%d: %.*s\n", r.file_path.c_str(),
                            r.line, static_cast<int>(line.size()), line.data());
            }
            ++shown;
        }
        if (shown > 0) {
            return 0;  // Named the import site(s); that is the answer.
        }
    }

    // (2) No import site found — offer nearest-name matches so a typo is
    // recoverable.
    print_symbol_suggestions(*client, symbol);

    return 0;
}

// -- refs command -------------------------------------------------------------

std::vector<bool> python_docstring_line_mask(
    const std::vector<std::string>& lines) {
    // Marks each line that begins INSIDE an open triple-quoted string
    // (`"""..."""` / `'''...'''`). These are the interior lines of a multi-line
    // Python docstring — the dominant source of `deprecated`-style natural-
    // language noise — which the single-line `ast_filters` classifiers cannot
    // detect because the opening quote lives on an earlier line. Same-line
    // triple quotes are left to `ast_filters` (which handles them per-column);
    // this mask only supplies the multi-line carry state ast_filters lacks.
    std::vector<bool> mask(lines.size(), false);
    bool in_triple = false;
    char tq = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        mask[i] = in_triple;  // interior lines carry the open-string state
        const std::string& ln = lines[i];
        size_t j = 0;
        while (j < ln.size()) {
            const char c = ln[j];
            if (in_triple) {
                if (c == tq && j + 2 < ln.size() && ln[j + 1] == tq &&
                    ln[j + 2] == tq) {
                    in_triple = false;
                    j += 3;
                    continue;
                }
                ++j;
                continue;
            }
            if (c == '#') break;  // comment runs to end-of-line
            if (c == '"' || c == '\'') {
                if (j + 2 < ln.size() && ln[j + 1] == c && ln[j + 2] == c) {
                    in_triple = true;
                    tq = c;
                    j += 3;
                    continue;
                }
                // Single-line string: skip to its unescaped closer so a quote
                // or `#` inside it doesn't spuriously toggle triple/comment.
                const char q = c;
                ++j;
                while (j < ln.size()) {
                    if (ln[j] == '\\') {
                        j += 2;
                        continue;
                    }
                    if (ln[j] == q) {
                        ++j;
                        break;
                    }
                    ++j;
                }
                continue;
            }
            ++j;
        }
    }
    return mask;
}

namespace {

// Reads `path` and returns the per-line docstring mask (index = line - 1).
// A file that cannot be read yields an empty mask, so its matches fall back to
// the single-line classifiers rather than being dropped.
std::vector<bool> docstring_mask_for_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return python_docstring_line_mask(lines);
}

}  // namespace

PartitionedReferences partition_references(
    const std::vector<ReferenceLocation>& refs) {
    PartitionedReferences out;
    // A file's docstring mask is computed once and reused across all of its
    // matches (a common-word symbol hits the same file many times).
    std::map<std::string, std::vector<bool>> mask_cache;
    for (const auto& r : refs) {
        // A match is lexical-only noise when it falls inside a comment, a
        // single-line string literal, OR an interior line of a multi-line
        // docstring. `context` is the exact source line and `column` the
        // 1-based match position (see handle_references). Everything else —
        // imports, calls, decorators, attribute accesses, plain identifiers —
        // is a real code reference and ranks first. A match we cannot classify
        // (empty line text) is kept as code-context, never silently hidden.
        bool in_docstring = false;
        if (!r.file_path.empty() && r.line > 0) {
            auto it = mask_cache.find(r.file_path);
            if (it == mask_cache.end()) {
                it = mask_cache
                         .emplace(r.file_path,
                                  docstring_mask_for_file(r.file_path))
                         .first;
            }
            const auto idx = static_cast<size_t>(r.line - 1);
            if (idx < it->second.size()) in_docstring = it->second[idx];
        }
        const bool lexical =
            in_docstring ||
            (!r.context.empty() &&
             (ast_filters::match_is_in_string_literal(r.context, r.column) ||
              ast_filters::match_is_in_comment(r.context, r.column)));
        (lexical ? out.lexical : out.code).push_back(r);
    }
    return out;
}

int run_refs(const GlobalFlags& flags, const std::string& symbol,
             bool json_output, bool show_all, bool count_only, bool terse,
             int max_results) {
    if (json_output) {
        std::cout
            << "Incorrect Usage: flag provided but not defined: -json\n\n"
            << "NAME:\n"
            << "   lci refs - Find symbol references\n\n"
            << "USAGE:\n"
            << "   lci refs command [command options] \n\n"
            << "COMMANDS:\n"
            << "   help, h  Shows a list of commands or help for one command\n\n"
            << "OPTIONS:\n"
            << "   --help, -h  show help\n";
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

    std::string refs_err;
    auto results = client->get_references(symbol, max_results, refs_err);
    if (!results) {
        std::cerr << "Error: references search failed: " << refs_err << "\n";
        return 1;
    }
    // A page exactly at the cap almost certainly means the server truncated;
    // say so everywhere (counts included) so a capped page is never mistaken
    // for a total (certified-absence discipline).
    const bool truncated =
        results->size() >= static_cast<size_t>(max_results);

    // Partition so real code references (imports/calls/decorators/attribute
    // accesses/plain identifiers) print FIRST and lexical-only matches (inside
    // strings/comments/docstrings) never outrank them. For a common-word symbol
    // like `deprecated`, the whole-text-search backend returns hundreds of
    // natural-language occurrences that would otherwise bury the real refs.
    PartitionedReferences parts = partition_references(*results);

    if (count_only) {
        size_t n = parts.code.size();
        if (show_all) n += parts.lexical.size();
        if (truncated) {
            std::printf("%zu+ (capped at %d, raise with --max)\n", n,
                        max_results);
        } else {
            std::printf("%zu\n", n);
        }
        return 0;
    }

    auto print_ref = [&](const ReferenceLocation& r) {
        if (terse) {
            std::printf("%s:%d\n", r.file_path.c_str(), r.line);
        } else if (!r.context.empty()) {
            std::printf("%s:%d: %s\n", r.file_path.c_str(), r.line,
                        r.context.c_str());
        } else {
            std::printf("%s:%d: %s\n", r.file_path.c_str(), r.line,
                        r.match_text.c_str());
        }
    };

    for (const auto& r : parts.code) {
        print_ref(r);
    }

    if (!parts.lexical.empty()) {
        const char* plural = parts.lexical.size() == 1 ? "" : "es";
        if (show_all) {
            std::printf("\n-- %zu lexical-only match%s in strings/comments --\n",
                        parts.lexical.size(), plural);
            for (const auto& r : parts.lexical) {
                print_ref(r);
            }
        } else {
            std::printf("%zu lexical-only match%s in strings/comments "
                        "(use --all to show)\n",
                        parts.lexical.size(), plural);
        }
    }

    if (truncated) {
        std::printf("-- results capped at %d; more may exist "
                    "(raise with --max) --\n",
                    max_results);
    }

    return 0;
}

int run_callers(const GlobalFlags& flags, const std::string& symbol,
                bool json_output, int max_callers) {
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

    std::string err;
    auto report = client->get_callers(symbol, max_callers, err);
    if (!report) {
        std::cerr << "Error: callers lookup failed: " << err << "\n";
        return 1;
    }

    if (json_output) {
        std::cout << report->dump(2) << "\n";
        return 0;
    }

    const auto& j = *report;
    const auto& defs = j["definitions"];
    if (defs.empty()) {
        std::printf("no callable definition of '%s' in the index\n",
                    symbol.c_str());
    } else {
        for (const auto& d : defs) {
            std::printf("callee: %s (%s) %s:%d\n",
                        d.value("name", "").c_str(),
                        d.value("type", "").c_str(),
                        d.value("file_path", "").c_str(), d.value("line", 0));
        }
    }

    const int total_callers = j.value("total_callers", 0);
    const int total_sites = j.value("total_call_sites", 0);
    std::printf("%d caller%s, %d call site%s\n", total_callers,
                total_callers == 1 ? "" : "s", total_sites,
                total_sites == 1 ? "" : "s");
    for (const auto& c : j["callers"]) {
        std::string lines;
        for (const auto& ln : c["call_lines"]) {
            if (!lines.empty()) lines += ",";
            lines += std::to_string(ln.get<int>());
        }
        std::printf("  %s %s:%d  calls at %s\n",
                    c.value("caller", "").c_str(),
                    c.value("file_path", "").c_str(), c.value("line", 0),
                    lines.c_str());
    }
    if (j.value("truncated", false)) {
        std::printf("  ... showing %zu of %d callers (raise --max)\n",
                    j["callers"].size(), total_callers);
    }

    // Unattributable sites: labeled, never mixed with confirmed callers.
    auto print_sites = [&](const char* key, const char* count_key,
                           const char* label) {
        if (!j.contains(key)) return;
        std::printf("%s: %d\n", label, j.value(count_key, 0));
        for (const auto& s : j[key]) {
            std::printf("  %s:%d%s%s\n", s.value("file_path", "").c_str(),
                        s.value("line", 0),
                        s.contains("caller") ? "  in " : "",
                        s.value("caller", "").c_str());
        }
        if (j.value(count_key, 0) > static_cast<int>(j[key].size())) {
            std::printf("  ... showing %zu of %d (raise --max)\n",
                        j[key].size(), j.value(count_key, 0));
        }
    };
    print_sites("dynamic_call_sites", "dynamic_count",
                "dynamic call sites (receiver type unknown at index time; "
                "not attributable statically)");
    print_sites("unresolved_call_sites", "unresolved_count",
                "unresolved call sites (target not in the index)");

    return 0;
}

// -- tree command -------------------------------------------------------------

namespace {

// Stamps `complexity` and `lines_of_code` onto every node in the tree
// rooted at `node`, looking up each node's enclosing symbol via the
// /browse-file endpoint. Cached per file so a tree with N nodes spread
// across F files makes at most F server round-trips, not N.
//
// Resilient to lookup failures: a file whose browse-file response is
// empty / errors out is left unannotated. The formatter then simply
// omits the metric segment for that node.
void annotate_tree_metrics(Client& client, nlohmann::json& node,
                           std::map<std::string, nlohmann::json>& cache) {
    if (!node.is_object()) return;
    std::string fp = node.value("file_path", "");
    std::string name = node.value("name", "");
    if (!fp.empty() && !name.empty()) {
        auto it = cache.find(fp);
        if (it == cache.end()) {
            BrowseFileRequest req;
            req.file = fp;
            req.max = 1000;
            std::string err;
            auto resp = client.browse_file(req, err);
            cache[fp] = resp.value_or(nlohmann::json());
            it = cache.find(fp);
        }
        if (!it->second.is_null() && it->second.is_object()) {
            auto syms = it->second.value("symbols",
                                          nlohmann::json::array());
            for (const auto& s : syms) {
                if (s.value("name", "") != name) continue;
                int cx = s.value("complexity", 0);
                int loc = s.value("lines_of_code", 0);
                if (cx > 0) node["complexity"] = cx;
                if (loc > 0) node["lines_of_code"] = loc;
                break;
            }
        }
    }
    if (node.contains("children") && node["children"].is_array()) {
        for (auto& child : node["children"]) {
            annotate_tree_metrics(client, child, cache);
        }
    }
}

}  // namespace

int run_tree(const GlobalFlags& flags, const std::string& function_name,
             int max_depth, bool show_lines, bool compact, bool json_output,
             bool agent_mode, bool metrics, const std::string& exclude) {
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

    auto start = std::chrono::steady_clock::now();

    TreeRequest req;
    req.function_name = function_name;
    req.max_depth = max_depth;
    req.show_lines = show_lines;
    req.compact = compact;
    req.exclude = exclude;
    req.agent_mode = agent_mode;

    std::string tree_err;
    auto tree = client->get_tree(req, tree_err);
    if (!tree) {
        std::cerr << "Error: " << tree_err << "\n";
        return 1;
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    double elapsed_ms =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
                .count()) /
        1000.0;

    // The /tree response is `{"tree": {...inner shape: root, root_function,
    // options, total_nodes, max_depth...}}` (server.cpp:991-993). Unwrap
    // once so both formatters and JSON output operate on the inner shape.
    nlohmann::json* inner = nullptr;
    if (tree->contains("tree") && (*tree)["tree"].is_object()) {
        inner = &(*tree)["tree"];
    } else if (tree->contains("root")) {
        // Defensive: a future server change might emit the inner shape
        // directly. Treat the whole response as the inner shape.
        inner = &(*tree);
    }

    // Stamp `complexity` and `lines_of_code` onto each node when --metrics
    // is on. We do this for both text and JSON output paths so the JSON
    // shape advertised to consumers is consistent regardless of mode.
    if (metrics && inner && inner->contains("root") &&
        (*inner)["root"].is_object()) {
        std::map<std::string, nlohmann::json> file_cache;
        annotate_tree_metrics(*client, (*inner)["root"], file_cache);
    }

    if (json_output) {
        // Reflect the user's display flags in the response options block
        // (the server returns a default block; we override the bits the
        // CLI controls so JSON consumers can introspect what was rendered).
        if (inner && inner->contains("options") &&
            (*inner)["options"].is_object()) {
            (*inner)["options"]["agent_mode"] = agent_mode;
            (*inner)["options"]["compact"] = compact;
            (*inner)["options"]["show_lines"] = show_lines;
            (*inner)["options"]["metrics"] = metrics;
        }
        nlohmann::json output;
        output["function"] = function_name;
        output["time_ms"] = elapsed_ms;
        output["tree"] = *tree;
        std::cout << output.dump(2) << "\n";
        return 0;
    }

    // Pick output mode. Compact wins over agent which wins over default
    // text -- mirrors Go's determineFormat (cmd/lci/main.go:1160) where
    // json > compact > text, with agent layered on top.
    tree_formatter::Options opts;
    if (compact) {
        opts.mode = tree_formatter::Mode::Compact;
    } else if (agent_mode) {
        opts.mode = tree_formatter::Mode::Agent;
    } else {
        opts.mode = tree_formatter::Mode::Text;
    }
    opts.show_lines = show_lines;
    opts.metrics = metrics;
    opts.max_depth = max_depth;

    std::printf("Function call tree for '%s' (generated in %.1fms)\n\n",
                function_name.c_str(), elapsed_ms);
    if (inner) {
        std::cout << tree_formatter::format_tree(*inner, opts);
    } else {
        std::cout << "No tree data available\n";
    }
    return 0;
}

// -- list command -------------------------------------------------------------

int run_list(const GlobalFlags& flags, bool verbose) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    // List files that *would* be indexed by walking the project root through
    // the same FileScanner used by the indexing pipeline. This matches the Go
    // implementation (cmd/lci listCommand → MasterIndex.ListFiles), which also
    // performs a stand-alone scan rather than querying the running server.
    //
    // Output contract (Go-compatible):
    //   - stdout: one absolute file path per line; verbose mode appends
    //     "(priority: N, size: B bytes)".
    //   - stderr: "\nTotal: N files would be indexed\n" summary in non-verbose
    //     mode (parity descriptors only capture stdout, but we keep it for
    //     interactive parity with the Go binary).
    FileScanner scanner(cfg);
    auto scan_result = scanner.scan();
    if (!scan_result.error.empty()) {
        std::cerr << "Error: " << scan_result.error << "\n";
        return 1;
    }
    if (scan_result.skipped_files > 0) {
        std::fprintf(stderr,
                     "Warning: corpus budget reached — %d lower-priority "
                     "files omitted from this listing\n",
                     scan_result.skipped_files);
    }
    auto tasks = std::move(scan_result.tasks);

    // FileScanner returns tasks sorted by indexing priority (desc) then path
    // (asc); the Go list command emits files in lexical scan order
    // (filepath.Walk), so re-sort by path here for parity.
    std::sort(tasks.begin(), tasks.end(),
              [](const FileTask& a, const FileTask& b) {
                  return a.path < b.path;
              });

    for (const auto& task : tasks) {
        if (verbose) {
            std::printf("%s (priority: %d, size: %lld bytes)\n",
                        task.path.c_str(), task.priority,
                        static_cast<long long>(task.size));
        } else {
            std::printf("%s\n", task.path.c_str());
        }
    }

    if (!verbose) {
        std::fprintf(stderr, "\nTotal: %zu files would be indexed\n",
                     tasks.size());
    }
    return 0;
}

}  // namespace cli
}  // namespace lci
