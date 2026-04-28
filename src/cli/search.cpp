#include <lci/cli/commands.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace lci {
namespace cli {

int run_search(const GlobalFlags& flags, const std::string& pattern,
               int max_lines, bool case_insensitive, bool json_output,
               bool light, bool compact_search, bool /*use_regex*/,
               const std::string& /*exclude_pattern*/,
               const std::string& /*include_pattern*/,
               bool /*invert_match*/, bool /*count_per_file*/,
               bool /*files_only*/, bool /*word_boundary*/,
               int /*max_count_per_file*/, bool /*include_ids*/,
               bool /*no_ids*/, bool /*comments_only*/, bool /*code_only*/,
               bool /*strings_only*/, const std::string& /*rank_by*/,
               const std::string& /*context_filter*/) {
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

    if (light) {
        std::cerr
            << "WARNING: --light flag is deprecated. Use 'lci grep' instead.\n\n";
    }

    auto start = std::chrono::steady_clock::now();

    std::string search_err;
    auto result = client->search(pattern, 500, case_insensitive, false,
                                 search_err);

    auto elapsed = std::chrono::steady_clock::now() - start;
    double elapsed_ms =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
                .count()) /
        1000.0;

    if (!result) {
        std::cerr << "Error: search failed: " << search_err << "\n";
        return 1;
    }

    auto& j = *result;

    if (json_output) {
        // Match Go's `lci search --json` wire format faithfully:
        //   - Each result element is wrapped in `{"result": {...}}` (Go's
        //     `searchtypes.StandardResult` has a `Result GrepResult
        //     json:"result"` tag — the wrapper is part of the contract).
        //   - Paths are emitted relative to cwd (Go calls
        //     `pathutil.ToRelativeStandardResults(results, projectRoot)`).
        //   - Top-level `mode` is "standard" (Go's standard-results path
        //     adds `"mode": "standard"`; integrated-mode would override).
        std::error_code rel_ec;
        auto cwd = std::filesystem::current_path(rel_ec);
        auto raw_results = j.value("results", nlohmann::json::array());
        nlohmann::json wrapped = nlohmann::json::array();
        for (auto& r : raw_results) {
            std::string path = r.value("path", "");
            if (!rel_ec && !path.empty()) {
                std::error_code ec;
                auto rel = std::filesystem::relative(path, cwd, ec);
                if (!ec) {
                    r["path"] = rel.string();
                }
            }
            wrapped.push_back(nlohmann::json{{"result", r}});
        }
        nlohmann::json output;
        output["query"] = pattern;
        output["time_ms"] = elapsed_ms;
        output["count"] = wrapped.size();
        output["results"] = wrapped;
        output["mode"] = "standard";
        std::cout << output.dump(2) << "\n";
        return 0;
    }

    auto results = j.value("results", nlohmann::json::array());

    if (compact_search) {
        std::printf("Found %zu matches in %.1fms (compact mode)\n\n",
                    results.size(), elapsed_ms);
        // Resolve the current working directory once so we can render
        // paths relative to it, mirroring Go's compact-mode formatter
        // (`b.py:1:` rather than `/abs/path/to/b.py:1:`).
        std::error_code rel_ec;
        auto cwd = std::filesystem::current_path(rel_ec);
        for (auto& r : results) {
            std::string path = r.value("path", "");
            int line = r.value("line", 0);
            auto context = r.value("context", nlohmann::json::object());
            int start_line = context.value("start_line", 0);
            auto lines = context.value("lines", nlohmann::json::array());

            std::string display_path = path;
            if (!rel_ec) {
                std::error_code rel2;
                auto rel = std::filesystem::relative(path, cwd, rel2);
                if (!rel2 && !rel.empty() &&
                    rel.string().find("..") == std::string::npos) {
                    display_path = rel.string();
                }
            }

            for (size_t i = 0; i < lines.size(); ++i) {
                int line_num = start_line + static_cast<int>(i);
                if (line_num == line) {
                    // Lines from context.lines may carry a trailing
                    // '\n' (the indexer preserves the file's final
                    // newline on the last line). printf adds its own
                    // newline below, so strip the trailing one to
                    // avoid an extra blank line in compact output.
                    std::string text = lines[i].get<std::string>();
                    if (!text.empty() && text.back() == '\n') text.pop_back();
                    std::printf("%s:%d: %s\n", display_path.c_str(), line_num,
                                text.c_str());
                    break;
                }
            }
        }
        return 0;
    }

    std::printf("Found %zu results in %.1fms (standard mode)\n\n",
                results.size(), elapsed_ms);

    for (auto& r : results) {
        std::string path = r.value("path", "");
        int line = r.value("line", 0);
        auto context = r.value("context", nlohmann::json::object());
        std::string block_name = context.value("block_name", "");
        std::string block_type = context.value("block_type", "");
        int start_line = context.value("start_line", 0);
        auto lines = context.value("lines", nlohmann::json::array());

        std::printf("%s:%d", path.c_str(), line);
        if (!block_name.empty()) {
            std::printf(" (in %s %s)", block_type.c_str(), block_name.c_str());
        }
        std::printf("\n");

        for (size_t i = 0; i < lines.size(); ++i) {
            int line_num = start_line + static_cast<int>(i);
            // Match Go's text-mode output exactly: a 6-char right-
            // padded line number + " | " + content + "\n". No '>'
            // marker on the match line — Go's formatter prints every
            // context line uniformly and parity is more valuable than
            // the marker. Strip any trailing '\n' on the source line
            // so printf's own newline doesn't double up.
            std::string text = lines[i].get<std::string>();
            if (!text.empty() && text.back() == '\n') text.pop_back();
            std::printf("%6d | %s\n", line_num, text.c_str());
        }
        // Go's standard-mode formatter prints two blank lines between
        // result blocks (one closing the context, one before the next
        // path). Match that spacing exactly.
        std::printf("\n\n");
    }

    (void)max_lines;
    return 0;
}

int run_grep(const GlobalFlags& flags, const std::string& pattern,
             int max_results, int /*context_lines*/, bool case_insensitive,
             bool json_output, const std::string& /*exclude_pattern*/,
             const std::string& /*include_pattern*/, bool /*exclude_tests*/,
             bool /*exclude_comments*/, bool /*use_regex*/) {
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

    std::string search_err;
    auto result =
        client->search(pattern, max_results, case_insensitive, false,
                       search_err);

    auto elapsed = std::chrono::steady_clock::now() - start;
    double elapsed_ms =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
                .count()) /
        1000.0;

    if (!result) {
        std::cerr << "Error: search failed: " << search_err << "\n";
        return 1;
    }

    auto& j = *result;

    if (json_output) {
        nlohmann::json output;
        output["query"] = pattern;
        output["time_ms"] = elapsed_ms;
        output["results"] = j.value("results", nlohmann::json::array());
        output["count"] =
            j.value("results", nlohmann::json::array()).size();
        output["mode"] = "grep";
        std::cout << output.dump(2) << "\n";
        return 0;
    }

    auto results = j.value("results", nlohmann::json::array());

    std::printf("Found %zu matches in %.1fms (grep mode)\n\n",
                results.size(), elapsed_ms);

    for (auto& r : results) {
        std::string path = r.value("path", "");
        int line = r.value("line", 0);
        int column = r.value("column", 0);
        auto context = r.value("context", nlohmann::json::object());
        int start_line = context.value("start_line", 0);
        auto lines = context.value("lines", nlohmann::json::array());

        for (size_t i = 0; i < lines.size(); ++i) {
            int line_num = start_line + static_cast<int>(i);
            if (line_num == line) {
                std::printf("%s:%d:%d:%s\n", path.c_str(), line_num, column,
                            lines[i].get<std::string>().c_str());
                break;
            }
        }
    }

    return 0;
}

}  // namespace cli
}  // namespace lci
