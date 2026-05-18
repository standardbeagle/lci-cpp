#include <lci/search/search_engine.h>

#include <lci/indexing/master_index.h>

#include <algorithm>
#include <cctype>

namespace lci {

// -- File classification ------------------------------------------------------

namespace {

bool has_extension(std::string_view path, std::string_view ext) {
    if (path.size() < ext.size()) return false;
    auto suffix = path.substr(path.size() - ext.size());
    for (size_t i = 0; i < ext.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(suffix[i])) !=
            std::tolower(static_cast<unsigned char>(ext[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view file_extension(std::string_view path) {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return {};
    return path.substr(dot);
}

std::string_view file_base(std::string_view path) {
    auto slash = path.rfind('/');
    if (slash == std::string_view::npos) {
        slash = path.rfind('\\');
    }
    if (slash == std::string_view::npos) return path;
    return path.substr(slash + 1);
}

bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.size() > haystack.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool is_code_extension(std::string_view ext) {
    static constexpr std::string_view exts[] = {
        ".go", ".rs", ".py", ".js", ".jsx", ".ts", ".tsx",
        ".java", ".c", ".cpp", ".cc", ".cxx", ".h", ".hpp",
        ".cs", ".php", ".rb", ".swift", ".kt", ".scala",
        ".lua", ".pl", ".pm", ".r", ".jl", ".ex", ".exs",
        ".erl", ".hrl", ".hs", ".clj", ".cljs", ".elm",
        ".vue", ".svelte", ".zig", ".nim", ".v", ".d", ".m", ".mm",
    };
    for (auto e : exts) {
        if (has_extension(ext, e)) return true;
    }
    return false;
}

bool is_doc_extension(std::string_view ext) {
    static constexpr std::string_view exts[] = {
        ".md", ".markdown", ".txt", ".rst", ".adoc",
        ".asciidoc", ".rdoc", ".org", ".wiki", ".textile",
    };
    for (auto e : exts) {
        if (has_extension(ext, e)) return true;
    }
    return false;
}

bool is_config_extension(std::string_view ext) {
    static constexpr std::string_view exts[] = {
        ".json", ".yaml", ".yml", ".toml", ".ini",
        ".cfg", ".conf", ".xml", ".kdl", ".env", ".properties",
    };
    for (auto e : exts) {
        if (has_extension(ext, e)) return true;
    }
    return false;
}

}  // namespace

FileCategory classify_file(std::string_view path) {
    auto base = file_base(path);

    if (contains_ci(base, "_test.") || contains_ci(base, ".test.") ||
        contains_ci(base, ".spec.") ||
        (base.size() >= 5 && contains_ci(base.substr(0, 5), "test_"))) {
        return FileCategory::Test;
    }

    auto ext = file_extension(path);
    if (is_code_extension(ext)) return FileCategory::Code;
    if (is_doc_extension(ext)) return FileCategory::Documentation;
    if (is_config_extension(ext)) return FileCategory::Config;
    return FileCategory::Unknown;
}

double score_file_type(std::string_view path) {
    auto cat = classify_file(path);
    switch (cat) {
        case FileCategory::Code: return kCodeFileBoost;
        case FileCategory::Documentation: return kDocFilePenalty;
        case FileCategory::Config: return kConfigFileBoost;
        case FileCategory::Test: return kCodeFileBoost * 0.8;
        case FileCategory::Unknown: return 0.0;
    }
    return 0.0;
}

bool is_test_file(std::string_view path) {
    return classify_file(path) == FileCategory::Test;
}

// -- SearchEngine -------------------------------------------------------------

SearchEngine::SearchEngine(MasterIndex& index)
    : index_(index),
      context_extractor_(index.file_content_store()) {}

std::vector<SearchResult> SearchEngine::search(
    const std::string& pattern, const SearchOptions& options) const {

    if (pattern.empty() || pattern.size() > 1000) return {};

    auto candidates = index_.find_candidate_files(pattern,
                                                   options.case_insensitive);
    if (candidates.empty()) {
        candidates = index_.get_all_file_ids();
    }
    if (candidates.empty()) return {};

    int effective_cap = options.max_results;
    if (effective_cap <= 0) {
        effective_cap = (static_cast<int>(candidates.size()) >= 400) ? 25 : 0;
    }

    std::vector<SearchResult> results;

    for (FileID fid : candidates) {
        if (effective_cap > 0 &&
            static_cast<int>(results.size()) >= effective_cap) {
            break;
        }
        process_file(fid, pattern, options, effective_cap, results);
    }

    // Score and rank results.
    for (auto& r : results) {
        r.score = score_result(r, pattern, false);
    }

    SearchCoordinator::rank(results);
    return results;
}

std::vector<SearchMatch> SearchEngine::find_matches(
    std::string_view content,
    std::string_view pattern,
    const SearchOptions& options) const {

    std::vector<SearchMatch> matches;
    if (pattern.empty() || content.empty()) return matches;

    // Per-query lowercase buffers are thread_local so case-insensitive
    // searches don't alloc per call (Karpathy rule 2 — no allocation in
    // inner loops). resize() shrinks when the next call's content is
    // smaller without freeing capacity.
    thread_local std::string lower_content;
    thread_local std::string lower_pattern;
    std::string_view search_content = content;
    std::string_view search_pattern = pattern;

    if (options.case_insensitive) {
        lower_content.resize(content.size());
        for (size_t i = 0; i < content.size(); ++i) {
            lower_content[i] = static_cast<char>(
                std::tolower(static_cast<unsigned char>(content[i])));
        }
        lower_pattern.resize(pattern.size());
        for (size_t i = 0; i < pattern.size(); ++i) {
            lower_pattern[i] = static_cast<char>(
                std::tolower(static_cast<unsigned char>(pattern[i])));
        }
        search_content = lower_content;
        search_pattern = lower_pattern;
    }

    size_t offset = 0;
    while (offset < search_content.size()) {
        auto found = search_content.find(search_pattern, offset);
        if (found == std::string_view::npos) break;

        int start = static_cast<int>(found);
        int end = start + static_cast<int>(pattern.size());

        if (options.word_boundary) {
            if (!is_word_boundary(content, start) ||
                !is_word_boundary(content, end)) {
                offset = found + 1;
                continue;
            }
        }

        bool exact = is_word_boundary(content, start) &&
                     is_word_boundary(content, end);

        matches.push_back({start, end, exact});
        offset = found + 1;
    }

    if (options.max_count_per_file > 0 &&
        static_cast<int>(matches.size()) > options.max_count_per_file) {
        matches.resize(static_cast<size_t>(options.max_count_per_file));
    } else if (static_cast<int>(matches.size()) > kMaxMatchesPerFile) {
        matches.resize(kMaxMatchesPerFile);
    }

    return matches;
}

double SearchEngine::score_result(const SearchResult& result,
                                   std::string_view pattern,
                                   bool has_symbol) const {
    double score = kBaseMatchScore;
    score += score_file_type(result.path);

    if (!has_symbol) {
        score += kNonSymbolPenalty;
    }

    score += static_cast<double>(calculate_pattern_complexity(pattern)) * 0.5;
    return score;
}

void SearchEngine::process_file(
    FileID file_id,
    std::string_view pattern,
    const SearchOptions& options,
    int effective_cap,
    std::vector<SearchResult>& results) const {

    auto content_sv = index_.file_content_store().get_content(file_id);
    if (content_sv.empty()) return;

    auto path = index_.id_to_path(file_id);

    if (options.exclude_tests && is_test_file(path)) return;

    auto matches = find_matches(content_sv, pattern, options);
    if (matches.empty()) return;

    // Fetch blocks for context extraction.
    auto file_symbols = index_.symbol_location_index().get_file_symbols(file_id);
    // Blocks come from FileInfo but we approximate with the file content store.
    // For block-aware context we need blocks; build empty for now.
    std::vector<BlockBoundary> blocks;

    // Deduplicate by line within this file.
    absl::flat_hash_set<int> seen_lines;

    for (const auto& match : matches) {
        if (effective_cap > 0 &&
            static_cast<int>(results.size()) >= effective_cap) {
            break;
        }

        int line = search_line_number(content_sv, match.start);

        if (seen_lines.contains(line)) continue;
        seen_lines.insert(line);

        int col = match.start - search_line_start(content_sv, match.start);

        std::string match_text;
        if (match.end > match.start &&
            match.end <= static_cast<int>(content_sv.size())) {
            match_text = std::string(
                content_sv.substr(static_cast<size_t>(match.start),
                                  static_cast<size_t>(match.end - match.start)));
        }

        SearchContext ctx;
        if (options.max_context_lines > 0) {
            ctx = context_extractor_.extract(file_id, blocks, line,
                                              options.max_context_lines);
        }

        results.push_back(SearchResult{
            file_id, std::move(path), line, col,
            std::move(match_text), 0.0, std::move(ctx)});

        // Path is moved, re-fetch for next iteration.
        path = index_.id_to_path(file_id);
    }
}

}  // namespace lci
