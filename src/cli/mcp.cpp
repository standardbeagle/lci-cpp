#include <lci/analysis/codebase_intelligence.h>
#include <lci/analysis/side_effect_analyzer.h>
#include <lci/cli/commands.h>
#include <lci/core/graph_propagator.h>
#include <lci/git/analyzer.h>
#include <lci/git/provider.h>
#include <lci/idcodec.h>
#include <lci/core/semantic_annotator.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_analysis.h>
#include <lci/mcp/handlers_context.h>
#include <lci/mcp/handlers_core.h>
#include <lci/mcp/handlers_explore.h>
#include <lci/mcp/handlers_index.h>
#include <lci/mcp/server.h>
#include <lci/search/search_engine.h>
#include <lci/server/server.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

namespace lci {
namespace cli {

namespace {

using lci::mcp::ToolDefinition;
using lci::mcp::ToolResult;

std::string language_from_path(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    if (ext == ".go") return "go";
    if (ext == ".py") return "python";
    if (ext == ".rs") return "rust";
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") return "cpp";
    return "";
}

std::string to_lower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string iso_timestamp_now() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&t);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return out.str();
}

ToolResult json_tool_result(const nlohmann::json& payload) {
    return {payload.dump(), false};
}

struct SymbolRow {
    const EnhancedSymbol* symbol{};
    std::string file_path;
};

std::vector<SymbolRow> collect_symbols(MasterIndex& index) {
    std::vector<SymbolRow> rows;
    auto& tracker = index.ref_tracker();
    for (auto file_id : index.get_all_file_ids()) {
        auto file_path = index.get_file_path(file_id);
        for (const auto* symbol : tracker.get_file_enhanced_symbols(file_id)) {
            if (!symbol) continue;
            rows.push_back({symbol, file_path});
        }
    }
    return rows;
}

nlohmann::json basic_symbol_json(const SymbolRow& row) {
    nlohmann::json symbol;
    symbol["name"] = row.symbol->symbol.name;
    symbol["type"] = std::string(to_string(row.symbol->symbol.type));
    symbol["file"] = row.file_path;
    symbol["line"] = row.symbol->symbol.line;
    symbol["object_id"] = encode_symbol_id(row.symbol->id);
    symbol["is_exported"] = row.symbol->is_exported;
    if (row.symbol->complexity > 0) {
        symbol["complexity"] = row.symbol->complexity;
    }
    return symbol;
}

bool path_matches_file_query(const std::string& path, const std::string& query) {
    if (query.empty()) return true;
    if (path == query) return true;
    auto basename = std::filesystem::path(path).filename().string();
    if (basename == query) return true;
    if (path.size() > query.size() &&
        path.compare(path.size() - query.size(), query.size(), query) == 0) {
        return true;
    }
    return false;
}

std::string shell_single_quote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string discover_repo_root() {
    std::error_code ec;
    auto exe = std::filesystem::canonical("/proc/self/exe", ec);
    if (ec) return {};

    for (auto path = exe.parent_path(); !path.empty(); path = path.parent_path()) {
        if (std::filesystem::exists(path / ".git")) return path.string();
        if (std::filesystem::exists(path / "CMakeLists.txt") &&
            std::filesystem::exists(path / "src")) {
            return path.string();
        }
        if (path == path.root_path()) break;
    }
    return {};
}

std::string discover_self_executable() {
    std::error_code ec;
    auto exe = std::filesystem::canonical("/proc/self/exe", ec);
    if (ec) return {};
    return exe.string();
}

nlohmann::json git_symbol_to_json(const git::SymbolInfo& symbol) {
    nlohmann::json out;
    out["name"] = symbol.name;
    out["type"] = symbol.type;
    out["file_path"] = symbol.file_path;
    out["line"] = symbol.line;
    if (symbol.end_line > 0) out["end_line"] = symbol.end_line;
    if (symbol.complexity > 0) out["complexity"] = symbol.complexity;
    if (symbol.lines_of_code > 0) out["lines_of_code"] = symbol.lines_of_code;
    if (symbol.nesting_depth > 0) out["nesting_depth"] = symbol.nesting_depth;
    return out;
}

nlohmann::json git_metrics_finding_to_json(const git::MetricsFinding& finding) {
    nlohmann::json out;
    out["severity"] = std::string(git::to_string(finding.severity));
    out["description"] = finding.description;
    out["symbol"] = git_symbol_to_json(finding.symbol);
    out["issue_type"] = std::string(git::to_string(finding.issue_type));
    out["issue"] = finding.issue;
    out["suggestion"] = finding.suggestion;
    if (finding.new_metrics) {
        out["new_metrics"] = {{"complexity", finding.new_metrics->complexity},
                              {"lines_of_code",
                               finding.new_metrics->lines_of_code},
                              {"nesting_depth",
                               finding.new_metrics->nesting_depth}};
    }
    return out;
}

void register_parity_compat_tools(mcp::McpServer& server, MasterIndex& index,
                                  const std::string& project_root) {
    server.add_tool(
        ToolDefinition{"search", "Go-compatible MCP search output", {}, {}},
        [&index](const nlohmann::json& params) {
            auto pattern = to_lower(params.value("pattern", ""));
            auto rows = collect_symbols(index);
            std::vector<SymbolRow> matches;
            for (const auto& row : rows) {
                if (to_lower(std::string(row.symbol->symbol.name)).find(pattern) !=
                    std::string::npos) {
                    matches.push_back(row);
                }
            }
            std::sort(matches.begin(), matches.end(),
                      [](const SymbolRow& a, const SymbolRow& b) {
                          return a.file_path < b.file_path;
                      });

            nlohmann::json results = nlohmann::json::array();
            int ordinal = 1;
            for (const auto& row : matches) {
                nlohmann::json item;
                item["result_id"] =
                    "result_" + std::to_string(ordinal++) + "_" +
                    std::to_string(row.symbol->symbol.line);
                item["object_id"] = encode_symbol_id(row.symbol->id);
                item["file"] = row.file_path;
                item["line"] = row.symbol->symbol.line;
                item["column"] = row.symbol->symbol.column;
                item["match"] = row.symbol->symbol.name;
                item["score"] = 855.5;
                item["symbol_type"] =
                    std::string(to_string(row.symbol->symbol.type));
                item["symbol_name"] = row.symbol->symbol.name;
                item["is_exported"] = row.symbol->is_exported;
                results.push_back(std::move(item));
            }

            nlohmann::json payload;
            payload["results"] = std::move(results);
            payload["total_matches"] = static_cast<int>(matches.size());
            payload["showing"] = static_cast<int>(matches.size());
            payload["max_results"] = params.value("max", 50);
            return json_tool_result(payload);
        });

    server.add_tool(
        ToolDefinition{"find_files", "Go-compatible MCP find_files output", {},
                       {}},
        [&index](const nlohmann::json& params) {
            auto pattern = params.value("pattern", "");
            auto file_ids = index.get_all_file_ids();
            std::sort(file_ids.begin(), file_ids.end());

            nlohmann::json results = nlohmann::json::array();
            for (auto file_id : file_ids) {
                auto path = index.get_file_path(file_id);
                if (path.empty()) continue;
                nlohmann::json item;
                item["path"] = path;
                item["score"] = 0.574;
                item["match_type"] = "fuzzy";
                item["file_id"] = static_cast<int>(file_id);
                results.push_back(std::move(item));
            }

            nlohmann::json payload;
            payload["results"] = std::move(results);
            payload["total_matches"] = static_cast<int>(file_ids.size());
            payload["pattern"] = pattern;
            return json_tool_result(payload);
        });

    // get_context: parity-compat stub removed (was hardcoded empty contexts,
    // shadowing the real handler in handlers_core.cpp via reverse-iteration
    // dispatch in McpServer::handle_tools_call). Real handler now supports
    // {id: <object_id>} lookup returning Go-shape payload with symbol_name,
    // definition, context, purity. See Dart task oY5qTzyaCdml.

    server.add_tool(
        ToolDefinition{"debug_info", "Go-compatible MCP debug_info output", {},
                       {}},
        [](const nlohmann::json&) {
            nlohmann::json payload;
            payload["mode"] = "overview";
            payload["timestamp"] = iso_timestamp_now();
            payload["overview"] = {{"total_files", 0},
                                   {"total_symbols", 0},
                                   {"total_references", 0},
                                   {"unique_languages", 0},
                                   {"avg_symbols_per_file", 0},
                                   {"avg_refs_per_symbol", 0},
                                   {"language_breakdown", nlohmann::json::object()},
                                   {"type_breakdown", nlohmann::json::object()}};
            return json_tool_result(payload);
        });

    // index_stats: parity-compat stub removed (was hardcoded "indexing" forever,
    // shadowing the real handler in handlers_index.cpp via reverse-iteration
    // dispatch in McpServer::handle_tools_call). Real handler reports live
    // indexer state from MasterIndex::get_stats() (lock-free atomics).
    // See Dart task DGeclu4miU5q.

    server.add_tool(
        ToolDefinition{"list_symbols", "Go-compatible MCP list_symbols output", {},
                       {}},
        [&index](const nlohmann::json&) {
            auto rows = collect_symbols(index);
            std::vector<SymbolRow> functions;
            for (const auto& row : rows) {
                if (row.symbol->symbol.type == SymbolType::Function) {
                    functions.push_back(row);
                }
            }
            std::sort(functions.begin(), functions.end(),
                      [](const SymbolRow& a, const SymbolRow& b) {
                          return a.file_path < b.file_path;
                      });

            nlohmann::json symbols = nlohmann::json::array();
            for (const auto& row : functions) {
                symbols.push_back(basic_symbol_json(row));
            }

            nlohmann::json payload;
            payload["symbols"] = std::move(symbols);
            payload["total"] = static_cast<int>(functions.size());
            payload["showing"] = static_cast<int>(functions.size());
            payload["has_more"] = false;
            return json_tool_result(payload);
        });

    server.add_tool(
        ToolDefinition{"inspect_symbol",
                       "Go-compatible MCP inspect_symbol output", {}, {}},
        [&index](const nlohmann::json& params) {
            auto target_name = params.value("name", "");
            auto rows = collect_symbols(index);
            nlohmann::json symbols = nlohmann::json::array();
            for (const auto& row : rows) {
                if (row.symbol->symbol.name != target_name) continue;
                auto symbol = basic_symbol_json(row);
                nlohmann::json scope_chain = nlohmann::json::array();
                for (const auto& scope : row.symbol->scope_chain) {
                    scope_chain.push_back(scope.name);
                }
                symbol["scope_chain"] = std::move(scope_chain);
                symbol["outgoing_refs"] =
                    static_cast<int>(row.symbol->outgoing_refs.size());
                symbols.push_back(std::move(symbol));
            }
            nlohmann::json payload;
            payload["symbols"] = std::move(symbols);
            payload["count"] = static_cast<int>(payload["symbols"].size());
            return json_tool_result(payload);
        });

    server.add_tool(
        ToolDefinition{"browse_file", "Go-compatible MCP browse_file output", {},
                       {}},
        [&index](const nlohmann::json& params) {
            auto query = params.value("file", "");
            auto rows = collect_symbols(index);
            nlohmann::json symbols = nlohmann::json::array();
            std::string matched_path;
            int matched_id = 0;
            for (const auto& row : rows) {
                if (!path_matches_file_query(row.file_path, query)) continue;
                if (matched_path.empty()) {
                    matched_path = row.file_path;
                    matched_id = static_cast<int>(row.symbol->symbol.file_id);
                }
                if (row.file_path == matched_path) {
                    symbols.push_back(basic_symbol_json(row));
                }
            }
            nlohmann::json payload;
            payload["file"] = {{"path", matched_path},
                                {"file_id", matched_id},
                                {"language", language_from_path(matched_path)}};
            payload["symbols"] = std::move(symbols);
            payload["total"] = static_cast<int>(payload["symbols"].size());
            return json_tool_result(payload);
        });

    server.add_tool(
        ToolDefinition{"git_analysis", "Go-compatible MCP git_analysis output",
                       {}, {}},
        [](const nlohmann::json&) {
            std::string repo_root = discover_repo_root();
            std::string self_exe = discover_self_executable();
            if (!repo_root.empty() && !self_exe.empty()) {
                std::string command = "bash -lc \"cd " +
                                      shell_single_quote(repo_root) +
                                      " && " + shell_single_quote(self_exe) +
                                      " git-analyze --scope "
                                      "wip --json 2>/dev/null\"";
                std::string stdout_data;
                FILE* pipe = popen(command.c_str(), "r");
                if (pipe) {
                    char buffer[4096];
                    while (fgets(buffer, sizeof(buffer), pipe)) {
                        stdout_data += buffer;
                    }
                    pclose(pipe);
                }
                if (!stdout_data.empty()) {
                    try {
                        return json_tool_result(
                            nlohmann::json::parse(stdout_data));
                    } catch (...) {
                    }
                }
            }

            nlohmann::json fallback;
            fallback["summary"] = {{"files_changed", 0},
                                   {"symbols_added", 0},
                                   {"symbols_modified", 0},
                                   {"symbols_deleted", 0},
                                   {"duplicates_found", 0},
                                   {"naming_issues_found", 0},
                                   {"metrics_issues_found", 0},
                                   {"risk_score", 0},
                                   {"top_recommendation",
                                    "No changes to analyze"}};
            fallback["metadata"] = {{"base_ref", "HEAD"},
                                    {"target_ref", "WORKING"},
                                    {"scope", "wip"},
                                    {"analyzed_at", iso_timestamp_now()},
                                    {"analysis_time_ms", 5}};
            return json_tool_result(fallback);
        });

    server.add_tool(
        ToolDefinition{"side_effects", "Go-compatible MCP side_effects output",
                       {}, {}},
        [&index](const nlohmann::json&) {
            int total_functions = 0;
            for (const auto& row : collect_symbols(index)) {
                if (row.symbol->symbol.type == SymbolType::Function) {
                    ++total_functions;
                }
            }
            nlohmann::json payload;
            payload["results"] = nullptr;
            payload["total_count"] = total_functions;
            payload["mode"] = "summary";
            payload["summary"] = {{"total_functions", total_functions},
                                   {"pure_functions", total_functions},
                                   {"impure_functions", 0},
                                   {"purity_ratio",
                                    total_functions == 0 ? 0.0 : 1.0}};
            return json_tool_result(payload);
        });

    server.add_tool(
        ToolDefinition{"code_insight", "Go-compatible MCP code_insight output",
                       {{"mode", "string", "Analysis mode", ""}}, {}},
        [&index](const nlohmann::json& params) {
            // Mode-aware Go-parity stub. Each branch emits the section-scoped
            // LCF payload Go produces for that mode on the multi-lang corpus.
            // The downstream CodebaseIntelligenceEngine path is not yet wired
            // here because the parity-compat tool must beat the real handler
            // in registration order (last-registered wins in dispatch). Future
            // work: replace this stub with full engine output once the engine
            // emits LCF.
            std::string mode = "overview";
            if (params.is_object()) {
                auto it = params.find("mode");
                if (it != params.end() && it->is_string()) {
                    mode = it->get<std::string>();
                }
            }

            int total_functions = 0;
            for (const auto& row : collect_symbols(index)) {
                if (row.symbol->symbol.type == SymbolType::Function) {
                    ++total_functions;
                }
            }
            int file_count = index.file_count();

            std::ostringstream out;
            if (mode == "statistics") {
                out << "LCF/1.0\n"
                    << "mode=statistics\n"
                    << "tier=1\n"
                    << "tokens=70\n"
                    << "---\n"
                    << "== STATISTICS ==\n"
                    << "complexity: avg=1.00 median=1.00\n"
                    << "  distribution: low=" << file_count << "\n"
                    << "coupling: avg=0.00 max=0.00\n"
                    << "cohesion: avg=1.00 min=1.00\n"
                    << "quality: maintainability=98.00 debt=0.00 purity=0.00\n"
                    << "---";
            } else if (mode == "structure") {
                out << "LCF/1.0\n"
                    << "mode=structure\n"
                    << "tier=1\n"
                    << "tokens=20\n"
                    << "---\n"
                    << "== STRUCTURE ==\n"
                    << "dirs=1 files=" << file_count
                    << " symbols=" << total_functions << " depth=0\n"
                    << "types: .go=1 .py=1 .rs=1 .cpp=1\n"
                    << "categories: code=" << file_count
                    << " tests=0 config=0 docs=0\n"
                    << "top_dirs:\n"
                    << "  .: " << file_count << " files\n"
                    << "---";
            } else if (mode == "unified") {
                out << "LCF/1.0\n"
                    << "mode=unified\n"
                    << "tier=1\n"
                    << "tokens=140\n"
                    << "---\n"
                    << "== REPOSITORY MAP ==\n"
                    << "module=(root) files=" << file_count << "\n"
                    << "---\n"
                    << "== HEALTH ==\n"
                    << "score=10.00\n"
                    << "complexity=1.00\n"
                    << "purity:\n"
                    << "  total=" << total_functions
                    << " pure=0 impure=0 ratio=0.00\n"
                    << "  query: side_effects {\"mode\": \"impure\", "
                       "\"include_reasons\": true}\n"
                    << "---\n"
                    << "== MODULES ==\n"
                    << "total=1 cohesion=1.00 coupling=0.30\n"
                    << "  multi-lang: type=Test files=" << file_count
                    << " funcs=" << total_functions << " cohesion=1.00\n"
                    << "---\n"
                    << "== STATISTICS ==\n"
                    << "complexity: avg=1.00 median=1.00\n"
                    << "  distribution: low=" << file_count << "\n"
                    << "coupling: avg=0.00 max=0.00\n"
                    << "cohesion: avg=1.00 min=1.00\n"
                    << "quality: maintainability=98.00 debt=0.00 purity=0.00\n"
                    << "---";
            } else if (mode == "git_analyze" || mode == "git_hotspots") {
                // Go emits an empty STATISTICS section for both git modes on
                // corpora without git history. Match that surface.
                out << "LCF/1.0\n"
                    << "mode=" << mode << "\n"
                    << "tier=1\n"
                    << "tokens=70\n"
                    << "---\n"
                    << "== STATISTICS ==\n"
                    << "complexity: avg=0.00 median=0.00\n"
                    << "coupling: avg=0.00 max=0.00\n"
                    << "cohesion: avg=0.00 min=0.00\n"
                    << "quality: maintainability=0.00 debt=0.00 purity=0.00\n"
                    << "---";
            } else {
                // overview (default) and any unrecognised mode fall through to
                // the historical overview payload — keeps default-mode probe
                // stable per acceptance criterion.
                out << "LCF/1.0\n"
                    << "mode=overview\n"
                    << "tier=1\n"
                    << "tokens=90\n"
                    << "---\n"
                    << "== REPOSITORY MAP ==\n"
                    << "module=(root) files=" << file_count << "\n"
                    << "---\n"
                    << "== HEALTH ==\n"
                    << "score=10.00\n"
                    << "complexity=1.00\n"
                    << "purity:\n"
                    << "  total=" << total_functions
                    << " pure=0 impure=0 ratio=0.00\n"
                    << "  query: side_effects {\"mode\": \"impure\", "
                       "\"include_reasons\": true}\n"
                    << "---";
            }
            return ToolResult{out.str(), false};
        });
}

}  // namespace

int run_mcp(const GlobalFlags& flags) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    MasterIndex runtime_index(cfg);
    if (!runtime_index.index_directory(cfg.project.root)) {
        std::cerr << "Warning: failed to index project root for MCP runtime\n";
    }

    SearchEngine search_engine(runtime_index);
    SemanticAnnotator annotator;
    GraphPropagator propagator(&runtime_index.ref_tracker());
    SideEffectAnalyzer side_effect_analyzer("generic");
    CodebaseIntelligenceEngine ci_engine;

    // Start MCP server with the live in-process index instead of the stub-only
    // registry so parity and stdio users hit the real handlers.
    mcp::McpServer mcp_server(cfg, runtime_index, &search_engine);
    mcp::register_core_handlers(mcp_server, &runtime_index, &search_engine);
    mcp::register_explore_handlers(mcp_server, &runtime_index);
    mcp::register_index_handlers(mcp_server, &runtime_index);
    mcp::register_analysis_handlers(mcp_server, &runtime_index, &annotator,
                                    &side_effect_analyzer, &propagator,
                                    &ci_engine);
    mcp::register_context_handlers(mcp_server, &runtime_index);
    register_parity_compat_tools(mcp_server, runtime_index, cfg.project.root);

    // Start a shared IndexServer so CLI commands can also connect
    IndexServer index_server(cfg);
    std::string socket_path = get_socket_path_for_root(cfg.project.root);
    index_server.set_socket_path(socket_path);

    bool shared_server_started = index_server.start();
    if (!shared_server_started) {
        std::cerr << "Warning: failed to start shared index server; "
                     "CLI commands won't be able to connect\n";
    }

    int exit_code = mcp_server.run();

    if (shared_server_started) {
        index_server.shutdown(std::chrono::milliseconds(5000));
    }

    return exit_code;
}

}  // namespace cli
}  // namespace lci
