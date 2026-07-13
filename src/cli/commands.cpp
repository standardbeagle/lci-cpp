#include <lci/cli/commands.h>

#include "name_aggregation.h"

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
    // recoverable. Pull a bounded candidate pool from the index and rank it
    // with the existing fuzzy matcher (jaro-winkler; 0.7 is the project's
    // fuzzy_threshold default, see SemanticScoringConfig).
    ListSymbolsRequest sym_req;
    sym_req.max = 500;
    std::string sym_err;
    if (auto listed = client->list_symbols(sym_req, sym_err)) {
        std::vector<std::string> candidates;
        if (listed->contains("symbols") && (*listed)["symbols"].is_array()) {
            candidates.reserve((*listed)["symbols"].size());
            for (const auto& s : (*listed)["symbols"]) {
                std::string name = s.value("name", "");
                if (!name.empty()) candidates.push_back(std::move(name));
            }
        }
        FuzzyMatcher matcher(/*enabled=*/true, /*threshold=*/0.7, "jaro-winkler");
        auto matches = matcher.find_matches(symbol, candidates);
        if (!matches.empty()) {
            constexpr size_t kMaxSuggestions = 5;
            std::printf("did you mean: ");
            std::set<std::string> shown_names;
            bool first = true;
            for (const auto& m : matches) {
                if (shown_names.size() >= kMaxSuggestions) break;
                if (!shown_names.insert(m.term).second) continue;
                std::printf("%s%s", first ? "" : ", ", m.term.c_str());
                first = false;
            }
            std::printf("?\n");
        }
    }

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
             bool json_output, bool show_all) {
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
    auto results = client->get_references(symbol, 100, refs_err);
    if (!results) {
        std::cerr << "Error: references search failed: " << refs_err << "\n";
        return 1;
    }

    // Partition so real code references (imports/calls/decorators/attribute
    // accesses/plain identifiers) print FIRST and lexical-only matches (inside
    // strings/comments/docstrings) never outrank them. For a common-word symbol
    // like `deprecated`, the whole-text-search backend returns hundreds of
    // natural-language occurrences that would otherwise bury the real refs.
    PartitionedReferences parts = partition_references(*results);

    auto print_ref = [](const ReferenceLocation& r) {
        if (!r.context.empty()) {
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
    auto tasks = scanner.scan();

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

// -- config commands ----------------------------------------------------------

int run_config_init(const GlobalFlags& /*flags*/, const std::string& format,
                    const std::string& output_arg, bool force, bool minimal) {
    std::string output = output_arg;
    if (output.empty()) {
        if (format == "kdl") {
            output = ".lci.kdl";
        } else if (format == "yaml") {
            output = ".lci.kdl";
        } else if (format == "json") {
            output = ".lci.kdl.json";
        } else {
            std::cerr << "Error: unsupported format: " << format << "\n";
            return 1;
        }
    }

    if (!force) {
        std::error_code ec;
        if (fs::exists(output, ec)) {
            std::cerr << "Error: configuration file " << output
                      << " already exists (use --force to overwrite)\n";
            return 1;
        }
    }

    std::string content;
    if (format == "kdl") {
        if (minimal) {
            content = R"(// Lightning Code Index Configuration
// Minimal configuration with commonly changed settings

index {
    max_total_size_mb 500          // Total indexed content limit
    max_file_count 10000           // Maximum number of files
    smart_size_control true        // Enable intelligent size management
    priority_mode "recent"         // Priority: "recent", "small", "important"
}

performance {
    max_memory_mb 500              // Memory limit for entire index
}

// Add project-specific exclusions
exclude {
    // "**/my-large-folder/**"
    // "**/*.generated.ts"
}

// Add additional file types to index
include {
    // "*.rs"                      // Rust files
    // "*.zig"                     // Zig files
}
)";
        } else {
            content = R"(// Lightning Code Index Configuration
// Full configuration template with all available options

project {
    name "my-project"
    root "."
}

index {
    max_file_size "10MB"           // Skip files larger than this
    max_total_size_mb 500          // Total indexed content limit
    max_file_count 10000           // Maximum number of files to index
    smart_size_control true        // Enable intelligent size management
    priority_mode "recent"         // Priority: "recent", "small", "important", "balanced"
    follow_symlinks false          // Don't follow symbolic links
}

performance {
    max_memory_mb 500              // Memory limit for entire index
    max_goroutines 8               // Parallel processing limit
    debounce_ms 100                // File change debouncing
}

search {
    max_results 100                // Limit search results
    max_context_lines 50           // Context around matches
    enable_fuzzy true              // Enable fuzzy matching
}

// Include specific file patterns (extends defaults)
include {
    "*.rs"                         // Rust files
    "*.zig"                        // Zig files
    "*.lua"                        // Lua scripts
}

// Exclude specific patterns (extends defaults)
// Note: All hidden directories (.*/) are excluded by default
exclude {
    "**/my-large-data/**"          // Project-specific exclusions
    "**/*.generated.ts"            // Generated TypeScript
}
)";
        }
    } else if (format == "json") {
        nlohmann::json cfg;
        cfg["version"] = 1;
        cfg["project"]["name"] = "my-project";
        cfg["project"]["root"] = ".";
        cfg["index"]["max_file_size"] = 10 * 1024 * 1024;
        cfg["index"]["max_total_size_mb"] = 500;
        cfg["index"]["max_file_count"] = 10000;
        cfg["index"]["follow_symlinks"] = false;
        cfg["index"]["smart_size_control"] = true;
        cfg["index"]["priority_mode"] = "recent";
        cfg["performance"]["max_memory_mb"] = 500;
        cfg["performance"]["max_goroutines"] = 8;
        cfg["performance"]["debounce_ms"] = 100;
        cfg["search"]["max_results"] = 100;
        cfg["search"]["max_context_lines"] = 50;
        cfg["search"]["enable_fuzzy"] = true;
        cfg["include"] = {"*.go", "*.js", "*.jsx", "*.ts", "*.tsx", "*.py"};
        cfg["exclude"] = {"**/.*/**", "**/node_modules/**", "**/vendor/**"};
        content = cfg.dump(2) + "\n";
    } else if (format == "yaml") {
        content = R"(version: 1
project:
  root: "."
  name: "my-project"
index:
  max_file_size: 10485760  # 10MB
  max_total_size_mb: 500
  max_file_count: 10000
  follow_symlinks: false
  smart_size_control: true
  priority_mode: "recent"
performance:
  max_memory_mb: 500
  max_goroutines: 8
  debounce_ms: 100
search:
  max_results: 100
  max_context_lines: 50
  enable_fuzzy: true
include:
  - "*.go"
  - "*.js"
  - "*.jsx"
  - "*.ts"
  - "*.tsx"
  - "*.py"
exclude:
  - "**/.*/**"
  - "**/node_modules/**"
  - "**/vendor/**"
)";
    } else {
        std::cerr << "Error: unsupported format: " << format << "\n";
        return 1;
    }

    std::ofstream ofs(output);
    if (!ofs) {
        std::cerr << "Error: failed to write config file: " << output << "\n";
        return 1;
    }
    ofs << content;
    ofs.close();

    std::printf("Configuration file created: %s\n", output.c_str());
    std::printf("Edit the file to customize settings for your project.\n");

    if (format == "kdl") {
        std::printf("\nCommon customizations:\n");
        std::printf(
            "  - Adjust memory limits: index.max_total_size_mb\n");
        std::printf(
            "  - Add project exclusions: exclude { \"**/my-folder/**\" }\n");
        std::printf(
            "  - Include additional languages: include { \"*.rs\" }\n");
    }

    return 0;
}

int run_config_show(const GlobalFlags& flags, const std::string& format) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }
    if (std::string err = validate_config(cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    if (format == "json") {
        nlohmann::json j;
        j["project"]["name"] = cfg.project.name;
        j["project"]["root"] = cfg.project.root;
        j["index"]["max_file_size"] = cfg.index.max_file_size;
        j["index"]["max_total_size_mb"] = cfg.index.max_total_size_mb;
        j["index"]["max_file_count"] = cfg.index.max_file_count;
        j["index"]["smart_size_control"] = cfg.index.smart_size_control;
        j["index"]["priority_mode"] = cfg.index.priority_mode;
        j["index"]["follow_symlinks"] = cfg.index.follow_symlinks;
        j["index"]["respect_gitignore"] = cfg.index.respect_gitignore;
        j["performance"]["max_memory_mb"] = cfg.performance.max_memory_mb;
        j["performance"]["debounce_ms"] = cfg.performance.debounce_ms;
        j["include"] = cfg.include;
        j["exclude"] = cfg.exclude;
        std::cout << j.dump(2) << "\n";
        return 0;
    }

    // Default: table format
    std::printf("Lightning Code Index Configuration\n");
    std::printf("=================================\n\n");

    std::printf("Project Settings:\n");
    std::printf("  Name:              %s\n", cfg.project.name.c_str());
    std::printf("  Root:              %s\n", cfg.project.root.c_str());
    std::printf("\n");

    std::printf("Index Settings:\n");
    std::printf("  Max file size:     %.1f MB\n",
                static_cast<double>(cfg.index.max_file_size) /
                    (1024.0 * 1024.0));
    std::printf("  Max total size:    %lld MB\n",
                static_cast<long long>(cfg.index.max_total_size_mb));
    std::printf("  Max file count:    %d\n", cfg.index.max_file_count);
    std::printf("  Smart size control: %s\n",
                cfg.index.smart_size_control ? "true" : "false");
    std::printf("  Priority mode:     %s\n",
                cfg.index.priority_mode.c_str());
    std::printf("  Follow symlinks:   %s\n",
                cfg.index.follow_symlinks ? "true" : "false");
    std::printf("  Respect .gitignore: %s\n",
                cfg.index.respect_gitignore ? "true" : "false");
    std::printf("\n");

    std::printf("Performance Settings:\n");
    std::printf("  Max memory:        %d MB\n", cfg.performance.max_memory_mb);
    std::printf("  Max goroutines:    %d\n", cfg.performance.max_goroutines);
    std::printf("  Debounce:          %d ms\n", cfg.performance.debounce_ms);
    std::printf("\n");

    std::printf("Search Settings:\n");
    std::printf("  Max results:       %d\n", cfg.search.max_results);
    std::printf("  Max context lines: %d\n", cfg.search.max_context_lines);
    std::printf("  Enable fuzzy:      %s\n",
                cfg.search.enable_fuzzy ? "true" : "false");
    std::printf("\n");

    std::printf("Include Patterns (%zu):\n", cfg.include.size());
    for (const auto& p : cfg.include) {
        std::printf("  %s\n", p.c_str());
    }
    std::printf("\n");

    std::printf("Exclude Patterns (%zu):\n", cfg.exclude.size());
    for (const auto& p : cfg.exclude) {
        std::printf("  %s\n", p.c_str());
    }

    return 0;
}

int run_config_validate(const GlobalFlags& flags) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::fprintf(stderr, "Configuration validation failed: %s\n",
                     err.c_str());
        return 1;
    }

    std::vector<std::string> warnings;

    if (cfg.performance.max_memory_mb < 100) {
        warnings.push_back(
            "MaxMemoryMB is very low (<100MB), may cause performance issues");
    }
    if (cfg.performance.max_memory_mb > 8000) {
        warnings.push_back(
            "MaxMemoryMB is very high (>8GB), ensure you have sufficient RAM");
    }
    if (cfg.index.max_total_size_mb < 50) {
        warnings.push_back(
            "MaxTotalSizeMB is very low (<50MB), may limit indexing "
            "capability");
    }
    if (cfg.index.max_file_count < 100) {
        warnings.push_back(
            "MaxFileCount is very low (<100), may limit indexing capability");
    }
    if (cfg.include.empty()) {
        warnings.push_back(
            "No include patterns specified, no files will be indexed");
    }

    std::printf("Configuration file is valid\n");
    std::printf("Config source: %s\n", flags.config_path.c_str());
    std::printf("Settings: %d files max, %dMB memory limit, %lldMB index "
                "limit\n",
                cfg.index.max_file_count, cfg.performance.max_memory_mb,
                static_cast<long long>(cfg.index.max_total_size_mb));

    if (!warnings.empty()) {
        std::printf("\nWarnings:\n");
        for (const auto& w : warnings) {
            std::printf("  - %s\n", w.c_str());
        }
    }

    return 0;
}

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
        std::cout << result->dump(2) << "\n";
        return 0;
    }

    // Text output matching Go implementation
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
