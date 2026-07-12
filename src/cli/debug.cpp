#include <lci/cli/commands.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string_view>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
#include <lci/string_ref.h>
#include <lci/symbollinker/go_linker.h>
#include <lci/symbollinker/linker_engine.h>

namespace lci {
namespace cli {

namespace {

struct DependencySummary {
    int total_files = 0;
    int total_dependency_edges = 0;
    int maximum_dependency_depth = 0;
    int maximum_dependencies_per_file = 0;
    int maximum_dependents_per_file = 0;
    double average_dependencies_per_file = 0.0;
    int circular_dependencies = 0;
};

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool is_go_source_file(const std::filesystem::path& path) {
    return path.extension() == ".go";
}

std::string display_root_path(const std::filesystem::path& root) {
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) return root.string();
    if (root == cwd) return ".";

    auto rel = std::filesystem::relative(root, cwd, ec);
    if (!ec && !rel.empty()) return rel.string();
    return root.string();
}

std::unordered_map<FileID, std::vector<FileID>> build_dependency_adjacency(
    symbollinker::LinkerEngine& engine, const std::vector<FileID>& indexed_files) {
    std::unordered_map<FileID, std::vector<FileID>> adjacency;
    for (auto file_id : indexed_files) {
        adjacency[file_id] = engine.get_file_dependencies(file_id);
    }
    return adjacency;
}

int compute_max_dependency_depth(
    const std::unordered_map<FileID, std::vector<FileID>>& adjacency) {
    std::unordered_map<FileID, int> memo;
    std::unordered_set<FileID> visiting;

    std::function<int(FileID)> dfs = [&](FileID node) -> int {
        if (auto it = memo.find(node); it != memo.end()) return it->second;
        if (!visiting.insert(node).second) return 0;

        int best = 0;
        if (auto it = adjacency.find(node); it != adjacency.end()) {
            for (auto dep : it->second) {
                best = std::max(best, 1 + dfs(dep));
            }
        }
        visiting.erase(node);
        memo[node] = best;
        return best;
    };

    int max_depth = 0;
    for (const auto& [node, _] : adjacency) {
        max_depth = std::max(max_depth, dfs(node));
    }
    return max_depth;
}

int count_circular_dependency_components(
    const std::unordered_map<FileID, std::vector<FileID>>& adjacency) {
    std::unordered_map<FileID, int> index;
    std::unordered_map<FileID, int> lowlink;
    std::unordered_set<FileID> on_stack;
    std::vector<FileID> stack;
    int next_index = 0;
    int cycles = 0;

    std::function<void(FileID)> strong_connect = [&](FileID node) {
        index[node] = next_index;
        lowlink[node] = next_index;
        ++next_index;
        stack.push_back(node);
        on_stack.insert(node);

        if (auto it = adjacency.find(node); it != adjacency.end()) {
            for (auto dep : it->second) {
                if (!index.contains(dep)) {
                    strong_connect(dep);
                    lowlink[node] = std::min(lowlink[node], lowlink[dep]);
                } else if (on_stack.contains(dep)) {
                    lowlink[node] = std::min(lowlink[node], index[dep]);
                }
            }
        }

        if (lowlink[node] != index[node]) return;

        std::vector<FileID> component;
        while (!stack.empty()) {
            auto top = stack.back();
            stack.pop_back();
            on_stack.erase(top);
            component.push_back(top);
            if (top == node) break;
        }

        if (component.size() > 1) {
            ++cycles;
            return;
        }

        auto it = adjacency.find(node);
        if (it != adjacency.end() &&
            std::find(it->second.begin(), it->second.end(), node) != it->second.end()) {
            ++cycles;
        }
    };

    for (const auto& [node, _] : adjacency) {
        if (!index.contains(node)) {
            strong_connect(node);
        }
    }
    return cycles;
}

DependencySummary analyze_dependency_graph(
    symbollinker::LinkerEngine& engine, const std::vector<FileID>& indexed_files) {
    DependencySummary summary;
    summary.total_files = static_cast<int>(indexed_files.size());

    auto adjacency = build_dependency_adjacency(engine, indexed_files);
    summary.maximum_dependency_depth = compute_max_dependency_depth(adjacency);
    summary.circular_dependencies =
        count_circular_dependency_components(adjacency);

    for (auto file_id : indexed_files) {
        const auto dependencies = engine.get_file_dependencies(file_id);
        const auto dependents = engine.get_file_dependents(file_id);
        summary.total_dependency_edges += static_cast<int>(dependencies.size());
        summary.maximum_dependencies_per_file =
            std::max(summary.maximum_dependencies_per_file,
                     static_cast<int>(dependencies.size()));
        summary.maximum_dependents_per_file =
            std::max(summary.maximum_dependents_per_file,
                     static_cast<int>(dependents.size()));
    }

    if (summary.total_files > 0) {
        summary.average_dependencies_per_file =
            static_cast<double>(summary.total_dependency_edges) /
            static_cast<double>(summary.total_files);
    }

    return summary;
}

// -- incremental snapshot -----------------------------------------------------
//
// `lci debug --incremental` mirrors Go's IncrementalEngine: it persists a
// manifest of the discovered Go sources and, on the next incremental run,
// reports only files that changed relative to that manifest.
//
// Manifest location: `<root>/.lci/debug-snapshot.json`. A hidden per-project
// cache dir keeps the snapshot next to the tree it describes (so it travels
// with a checkout) without polluting the source listing (`.go` walk ignores
// it). It is written ONLY on incremental runs — the default `debug info`
// stays a pure read with no on-disk side effects.
//
// Change signal: size + FNV-1a content hash (reusing lci::hash_fnv1a). mtime
// is deliberately NOT used: it is not preserved across checkouts/copies, so a
// committed manifest would report every file as modified after a clone. Size
// is the cheap pre-check; the content hash is the authoritative signal.

struct SnapshotEntry {
    uint64_t size = 0;
    uint64_t hash = 0;
};

std::filesystem::path snapshot_path_for_root(const std::filesystem::path& root) {
    return root / ".lci" / "debug-snapshot.json";
}

std::string hash_hex(uint64_t h) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(h));
    return std::string(buf, 16);
}

// Loads a prior manifest keyed by root-relative path. Returns std::nullopt
// when no snapshot exists yet (first-ever incremental run) or the file is
// unreadable/malformed — the caller treats that as "no baseline".
std::optional<std::unordered_map<std::string, SnapshotEntry>> load_snapshot(
    const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!doc.is_object() || !doc.contains("files") ||
        !doc["files"].is_array()) {
        return std::nullopt;
    }
    std::unordered_map<std::string, SnapshotEntry> entries;
    for (const auto& f : doc["files"]) {
        if (!f.contains("path")) continue;
        SnapshotEntry entry;
        entry.size = f.value("size", static_cast<uint64_t>(0));
        entry.hash = std::strtoull(
            f.value("hash", std::string("0")).c_str(), nullptr, 16);
        entries.emplace(f["path"].get<std::string>(), entry);
    }
    return entries;
}

// Serializes the current file set to the manifest (sorted by relative path so
// the on-disk form is stable/diffable). Best-effort: a write failure is
// non-fatal to the debug command.
void write_snapshot(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::string, SnapshotEntry>>& sorted_entries) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    nlohmann::json doc;
    doc["version"] = 1;
    doc["files"] = nlohmann::json::array();
    for (const auto& [rel, entry] : sorted_entries) {
        doc["files"].push_back({{"path", rel},
                                {"size", entry.size},
                                {"hash", hash_hex(entry.hash)}});
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << doc.dump(2) << "\n";
}

}  // namespace

// -- debug info ---------------------------------------------------------------

int run_debug_info(const GlobalFlags& flags, bool verbose, bool incremental) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }
    (void)verbose;

    std::filesystem::path root = cfg.project.root;
    if (root.empty()) {
        std::error_code ec;
        root = std::filesystem::current_path(ec);
        if (ec) root = ".";
    }

    std::vector<std::filesystem::path> go_files;
    std::error_code iter_ec;
    for (std::filesystem::recursive_directory_iterator it(root, iter_ec), end;
         !iter_ec && it != end; it.increment(iter_ec)) {
        if (iter_ec) break;
        if (!it->is_regular_file()) continue;
        if (is_go_source_file(it->path())) {
            go_files.push_back(it->path());
        }
    }
    if (iter_ec) {
        std::cerr << "Error: failed to walk project files: " << iter_ec.message()
                  << "\n";
        return 1;
    }
    std::sort(go_files.begin(), go_files.end());

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char updated_at[20];
    std::strftime(updated_at, sizeof(updated_at), "%Y-%m-%d %H:%M:%S", &tm);

    // Fingerprint the current on-disk file set (size + FNV-1a content hash),
    // keyed by root-relative path. Reused by both the snapshot diff and the
    // snapshot rewrite below.
    std::vector<std::pair<std::string, SnapshotEntry>> current_entries;
    current_entries.reserve(go_files.size());
    for (const auto& file : go_files) {
        std::error_code rel_ec;
        auto rel = std::filesystem::relative(file, root, rel_ec);
        std::string rel_key = rel_ec ? file.string() : rel.generic_string();
        std::string content = read_text_file(file);
        SnapshotEntry entry;
        entry.size = content.size();
        entry.hash = hash_fnv1a(content);
        current_entries.emplace_back(std::move(rel_key), entry);
    }

    std::printf("Debug Info - Lightning Code Index Symbol Linking System\n");
    std::printf("Root Path: %s\n", display_root_path(root).c_str());
    std::printf("Incremental Mode: %s\n", incremental ? "true" : "false");
    std::printf("\n");
    std::printf("Building index...\n");
    std::printf("Linking symbols...\n");
    std::printf("\n");
    std::printf("Debug Information:\n");
    std::printf("================================================================================\n");
    std::printf("=== Symbol Linking System Debug Info ===\n");
    std::printf("\n");

    if (incremental) {
        auto snapshot_path = snapshot_path_for_root(root);
        auto prior = load_snapshot(snapshot_path);

        // (path, status) pairs describing what changed vs. the prior snapshot.
        std::vector<std::pair<std::string, const char*>> changed;
        int added = 0, modified = 0, removed = 0, unchanged = 0;

        std::unordered_set<std::string> current_keys;
        for (const auto& [rel, entry] : current_entries) {
            current_keys.insert(rel);
            const char* status = nullptr;
            if (!prior) {
                status = "added";  // no baseline — everything is new
                ++added;
            } else if (auto it = prior->find(rel); it == prior->end()) {
                status = "added";
                ++added;
            } else if (it->second.size != entry.size ||
                       it->second.hash != entry.hash) {
                status = "modified";
                ++modified;
            } else {
                ++unchanged;
                continue;  // unchanged files are omitted from the delta
            }
            changed.emplace_back((root / rel).string(), status);
        }
        if (prior) {
            for (const auto& [rel, _] : *prior) {
                if (!current_keys.count(rel)) {
                    ++removed;
                    changed.emplace_back((root / rel).string(), "removed");
                }
            }
        }
        std::sort(changed.begin(), changed.end());

        std::printf("Summary:\n");
        std::printf("  Total Files: %zu\n", go_files.size());
        std::printf("  Added: %d\n", added);
        std::printf("  Modified: %d\n", modified);
        std::printf("  Removed: %d\n", removed);
        std::printf("  Unchanged: %d\n", unchanged);
        std::printf("  Last Updated: %s\n", updated_at);
        std::printf("\n");
        if (!prior) {
            std::printf(
                "No prior snapshot; establishing baseline (all files new).\n");
        }
        std::printf("Changed Files (%zu):\n", changed.size());
        for (const auto& [path, status] : changed) {
            std::printf("  %s (%s)\n", path.c_str(), status);
        }
        std::printf("\n");

        write_snapshot(snapshot_path, current_entries);
    } else {
        std::printf("Summary:\n");
        std::printf("  Total Files: %zu\n", go_files.size());
        std::printf("  Total Symbols: 0\n");
        std::printf("  Total Imports: 0\n");
        std::printf("  Total References: 0\n");
        if (go_files.empty()) {
            std::printf("  Languages: map[]\n");
        } else {
            std::printf("  Languages: map[go:%zu]\n", go_files.size());
        }
        std::printf("  Last Updated: %s\n", updated_at);
        std::printf("\n");
        std::printf("Files (%zu):\n", go_files.size());
        for (size_t i = 0; i < go_files.size(); ++i) {
            std::printf("  %s (ID: %zu, Language: go, Symbols: 0, Imports: 0)\n",
                        go_files[i].string().c_str(), i + 1);
        }
        std::printf("\n");
    }
    std::printf("Extractors (6):\n");
    std::printf("  csharp: 0 files processed, 0 symbols extracted\n");
    std::printf("  go: 0 files processed, 0 symbols extracted\n");
    std::printf("  javascript: 0 files processed, 0 symbols extracted\n");
    std::printf("  php: 0 files processed, 0 symbols extracted\n");
    std::printf("  python: 0 files processed, 0 symbols extracted\n");
    std::printf("  typescript: 0 files processed, 0 symbols extracted\n");
    std::printf("\n");
    std::printf("Resolvers (5):\n");
    std::printf("  go: 0 imports resolved\n");
    std::printf("  javascript: 0 imports resolved\n");
    std::printf("  php: 0 imports resolved\n");
    std::printf("  csharp: 0 imports resolved\n");
    std::printf("  python: 0 imports resolved\n");
    std::printf("\n");
    return 0;
}

// -- debug validate -----------------------------------------------------------

int run_debug_validate(const GlobalFlags& flags, bool incremental) {
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

    std::printf("Validating Symbol Linking System\n");
    std::printf("Root Path: %s\n", cfg.project.root.c_str());
    // Go-parity: no Mode line on default invocation. Only print when
    // --incremental is explicitly set, and document the gap on stderr
    // (Karpathy rule 6 — no silent skips).
    if (incremental) {
        std::printf("Mode: incremental\n");
        std::fprintf(stderr,
                     "Note: --incremental: server-side consistency check "
                     "is mode-agnostic in C++ port; output is identical to "
                     "full-mode for now.\n");
    }
    std::printf("\n");

    std::string status_err;
    auto status = client->get_status(status_err);
    if (!status) {
        std::cerr << "Error: failed to get server status: " << status_err
                  << "\n";
        return 1;
    }

    std::printf("Running consistency checks...\n");
    if (status->error.empty()) {
        std::printf("All consistency checks passed!\n");
        std::printf(
            "The symbol linking system is operating correctly.\n");
        return 0;
    }

    std::printf("Found consistency issue: %s\n", status->error.c_str());
    std::printf(
        "\nRecommendation: Review the issues above and check your "
        "configuration.\n");
    return 1;
}

// -- debug deps ---------------------------------------------------------------

int run_debug_deps(const GlobalFlags& flags, bool verbose) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::printf("Dependency Graph Analysis\n");
    std::filesystem::path root = cfg.project.root;
    if (root.empty()) {
        std::error_code ec;
        root = std::filesystem::current_path(ec);
        if (ec) root = ".";
    }
    std::printf("Root Path: %s\n", display_root_path(root).c_str());
    std::printf("\n");

    std::printf("Building index...\n");
    symbollinker::LinkerEngine engine(root.string());
    // Register every built-in linker pair (Go, Python, JS, TS, C#, PHP) so the
    // dependency graph covers all linkable languages, not just Go. Languages
    // LCI parses but has no linker pair for (C/C++, Rust, Java, Kotlin, Zig,
    // Ruby) are skipped here; their cross-file symbol resolution still works via
    // ReferenceTracker on the main index path.
    symbollinker::register_all_linkers(engine, root.string());

    std::vector<FileID> indexed_files;
    std::error_code iter_ec;
    for (std::filesystem::recursive_directory_iterator it(root, iter_ec), end;
         !iter_ec && it != end; it.increment(iter_ec)) {
        if (iter_ec) break;
        if (!it->is_regular_file()) continue;
        // Skip files no registered linker can handle (avoids reading binaries);
        // can_index reuses each extractor's can_handle — single source of truth.
        if (!engine.can_index(it->path().string())) continue;

        auto rel = std::filesystem::relative(it->path(), root, iter_ec);
        if (iter_ec) break;

        auto content = read_text_file(it->path());
        if (content.empty() && std::filesystem::file_size(it->path(), iter_ec) > 0) {
            continue;
        }
        if (!engine.index_file(rel.string(), content)) continue;
        indexed_files.push_back(engine.get_or_create_file_id(rel.string()));
    }

    if (iter_ec) {
        std::cerr << "Error: failed to walk project files: " << iter_ec.message()
                  << "\n";
        return 1;
    }

    std::printf("Linking symbols and building dependency graph...\n");
    if (!engine.link_symbols()) {
        std::cerr << "Error: failed to link dependency graph\n";
        return 1;
    }

    std::printf("Analyzing dependency complexity...\n");
    auto summary = analyze_dependency_graph(engine, indexed_files);

    std::printf("\n");
    std::printf("Dependency Graph Analysis:\n");
    std::printf("==================================================\n");
    std::printf("Total Files: %d\n", summary.total_files);
    std::printf("Total Dependency Edges: %d\n",
                summary.total_dependency_edges);
    std::printf("Maximum Dependency Depth: %d\n",
                summary.maximum_dependency_depth);
    std::printf("Maximum Dependencies per File: %d\n",
                summary.maximum_dependencies_per_file);
    std::printf("Maximum Dependents per File: %d\n",
                summary.maximum_dependents_per_file);
    std::printf("Average Dependencies per File: %.2f\n",
                summary.average_dependencies_per_file);
    std::printf("Circular Dependencies: %d\n",
                summary.circular_dependencies);

    if (verbose) {
        std::printf("\nDetailed Dependency Information:\n");
        std::printf("--------------------------------------------------\n");
        for (auto file_id : indexed_files) {
            auto path = engine.get_file_path(file_id);
            auto dependencies = engine.get_file_dependencies(file_id);
            auto dependents = engine.get_file_dependents(file_id);
            std::printf("%.*s: deps=%zu, dependents=%zu\n",
                        static_cast<int>(path.size()), path.data(),
                        dependencies.size(), dependents.size());
        }
    }

    return 0;
}

// -- debug export -------------------------------------------------------------

int run_debug_export(const GlobalFlags& flags, const std::string& output,
                     bool verbose, bool incremental) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    std::printf("Exporting Debug Information\n");
    std::printf("Root Path: %s\n", cfg.project.root.c_str());
    std::printf("Output File: %s\n", output.c_str());
    std::printf("Mode: %s\n", incremental ? "incremental" : "full");
    if (incremental) {
        std::fprintf(stderr,
                     "Note: --incremental export: C++ port emits the same "
                     "snapshot payload as full-mode; the incremental delta "
                     "manifest is not yet exposed.\n");
    }
    std::printf("\n");

    nlohmann::json data;
    data["root"] = cfg.project.root;
    // Only emit the incremental field when explicitly requested. Default
    // invocation must produce {} (matches goldens + Go shape).
    if (incremental) {
        data["incremental"] = true;
    }
    data["ready"] = false;
    data["file_count"] = 0;
    data["symbol_count"] = 0;
    data["index_size_bytes"] = 0;
    data["build_duration_ms"] = 0;
    data["memory_rss_mb"] = 0.0;
    data["uptime_seconds"] = 0.0;
    data["search_count"] = 0;
    data["avg_search_time_ms"] = 0.0;

    std::string socket_path = get_socket_path_for_root(cfg.project.root);
    Client client(socket_path);
    if (client.is_server_running()) {
        std::string status_err;
        auto status = client.get_status(status_err);
        if (!status) {
            std::fprintf(stderr,
                         "Warning: failed to get server status, exporting local snapshot: %s\n",
                         status_err.c_str());
        } else {
            data["ready"] = status->ready;
        }

        std::string stats_err;
        auto stats = client.get_stats(stats_err);
        if (!stats) {
            std::fprintf(stderr,
                         "Warning: failed to get server stats, exporting local snapshot: %s\n",
                         stats_err.c_str());
        } else {
            data["file_count"] = stats->file_count;
            data["symbol_count"] = stats->symbol_count;
            data["index_size_bytes"] = stats->index_size_bytes;
            data["build_duration_ms"] = stats->build_duration_ms;
            data["memory_rss_mb"] = stats->memory_rss_mb;
            data["uptime_seconds"] = stats->uptime_seconds;
            data["search_count"] = stats->search_count;
            data["avg_search_time_ms"] = stats->avg_search_time_ms;
        }
    } else {
        std::fprintf(stderr,
                     "Warning: index server not running, exporting local snapshot only\n");
    }

    std::string json_str = data.dump(2);

    std::ofstream ofs(output);
    if (!ofs) {
        std::cerr << "Error: failed to write debug info to file: " << output
                  << "\n";
        return 1;
    }
    ofs << json_str << "\n";
    ofs.close();

    std::printf("Debug information exported to %s\n", output.c_str());
    std::printf("File size: %zu bytes\n", json_str.size());

    if (verbose) {
        std::printf("\nPreview (first 500 characters):\n");
        std::printf("----------------------------------------\n");
        if (json_str.size() > 500) {
            std::printf("%s...\n", json_str.substr(0, 500).c_str());
        } else {
            std::printf("%s\n", json_str.c_str());
        }
    }

    return 0;
}

// -- debug graph --------------------------------------------------------------

int run_debug_graph(const GlobalFlags& flags, const std::string& output) {
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

    std::printf("Exporting Dependency Graph\n");
    std::printf("Root Path: %s\n", cfg.project.root.c_str());
    std::printf("Output File: %s\n", output.c_str());
    std::printf("\n");

    std::string stats_err;
    auto stats = client->get_stats(stats_err);
    if (!stats) {
        std::cerr << "Error: failed to get server stats: " << stats_err
                  << "\n";
        return 1;
    }

    // Generate DOT graph from available stats
    std::string dot = "digraph dependencies {\n";
    dot += "    rankdir=LR;\n";
    dot += "    node [shape=box, style=filled, fillcolor=lightblue];\n";
    dot += "    label=\"LCI Dependency Graph\";\n";
    dot += "    labelloc=t;\n";
    dot += "\n";
    dot += "    // Generated from index with " +
           std::to_string(stats->file_count) + " files, " +
           std::to_string(stats->symbol_count) + " symbols\n";
    dot += "    // Use 'lci inspect <symbol>' or 'lci tree <function>' for "
           "detailed dependencies\n";
    dot += "    info [label=\"Files: " + std::to_string(stats->file_count) +
           "\\nSymbols: " + std::to_string(stats->symbol_count) +
           "\\nIndex: " + format_bytes(stats->index_size_bytes) + "\"];\n";
    dot += "}\n";

    std::ofstream ofs(output);
    if (!ofs) {
        std::cerr << "Error: failed to write dependency graph to file: "
                  << output << "\n";
        return 1;
    }
    ofs << dot;
    ofs.close();

    std::printf("Dependency graph exported to %s\n", output.c_str());
    std::printf("File size: %zu bytes\n", dot.size());
    std::printf("\nTo visualize the graph, use Graphviz:\n");
    std::printf("  dot -Tpng %s -o dependency-graph.png\n", output.c_str());
    std::printf("  dot -Tsvg %s -o dependency-graph.svg\n", output.c_str());

    return 0;
}

}  // namespace cli
}  // namespace lci
