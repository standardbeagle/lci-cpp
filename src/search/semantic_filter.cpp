#include <lci/search/semantic_filter.h>

#include <algorithm>
#include <cctype>

namespace lci {

// -- SemanticFilter -----------------------------------------------------------

SemanticFilter::SemanticFilter(const FileContentStore& store)
    : store_(store) {}

std::vector<SemanticMatch> SemanticFilter::apply_filter(
    FileID /*file_id*/,
    std::string_view content,
    const std::vector<SymbolLineEntry>& symbol_map,
    const std::vector<SemanticMatch>& matches,
    std::string_view pattern,
    const SearchOptions& options) const {

    // No semantic filtering requested -- return all matches.
    if (!options.declaration_only && !options.usage_only &&
        !options.exclude_comments) {
        return matches;
    }

    std::vector<SemanticMatch> filtered;
    filtered.reserve(matches.size());

    for (const auto& match : matches) {
        if (passes_filter(content, symbol_map, match, pattern, options)) {
            filtered.push_back(match);
        }
    }

    return filtered;
}

bool SemanticFilter::passes_filter(
    std::string_view content,
    const std::vector<SymbolLineEntry>& symbol_map,
    const SemanticMatch& match,
    std::string_view pattern,
    const SearchOptions& options) const {

    int line = match.line;

    // Exclude comments filter.
    if (options.exclude_comments) {
        int line_start = 0;
        int current_line = 1;
        for (size_t i = 0; i < content.size(); ++i) {
            if (current_line == line) {
                line_start = static_cast<int>(i);
                break;
            }
            if (content[i] == '\n') ++current_line;
        }
        // Find line end.
        size_t line_end = content.find('\n',
            static_cast<size_t>(line_start));
        if (line_end == std::string_view::npos) {
            line_end = content.size();
        }
        auto line_text = content.substr(
            static_cast<size_t>(line_start),
            line_end - static_cast<size_t>(line_start));
        if (is_comment_line(line_text)) return false;
    }

    // Find symbol at this location.
    auto* symbol = find_symbol_at_line(symbol_map, line, pattern);

    if (symbol != nullptr) {
        if (options.declaration_only) return true;
        if (options.usage_only) return false;
    } else {
        if (options.declaration_only) return false;
        if (options.usage_only) return true;
    }

    return true;
}

const SymbolLineEntry* SemanticFilter::find_symbol_at_line(
    const std::vector<SymbolLineEntry>& symbol_map,
    int line,
    std::string_view pattern) const {

    // First try exact match on pattern name and line.
    if (!pattern.empty()) {
        for (const auto& entry : symbol_map) {
            if (entry.line == line && entry.name == pattern) {
                return &entry;
            }
        }
    }

    // Fall back to any symbol on this line.
    for (const auto& entry : symbol_map) {
        if (entry.line == line) return &entry;
    }

    return nullptr;
}

bool SemanticFilter::is_comment_line(std::string_view line) {
    // Trim leading whitespace.
    size_t start = 0;
    while (start < line.size() &&
           (line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }
    auto trimmed = line.substr(start);
    if (trimmed.empty()) return false;

    if (trimmed.size() >= 2 && trimmed[0] == '/' && trimmed[1] == '/') {
        return true;
    }
    if (trimmed[0] == '#') return true;
    if (trimmed.size() >= 2 && trimmed[0] == '/' && trimmed[1] == '*') {
        return true;
    }
    if (trimmed.find("*/") != std::string_view::npos) return true;

    return false;
}

int SemanticFilter::line_for_offset(std::string_view content, int offset) {
    int line = 1;
    for (int i = 0; i < offset && i < static_cast<int>(content.size()); ++i) {
        if (content[static_cast<size_t>(i)] == '\n') ++line;
    }
    return line;
}

bool SemanticFilter::is_exported_symbol(const SymbolLineEntry& entry) {
    if (entry.is_exported) return true;
    if (entry.name.empty()) return false;
    return entry.name[0] >= 'A' && entry.name[0] <= 'Z';
}

bool SemanticFilter::contains_case_insensitive(
    const std::vector<std::string>& haystack, std::string_view needle) {
    for (const auto& item : haystack) {
        if (item.size() != needle.size()) continue;
        bool match = true;
        for (size_t i = 0; i < item.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(item[i])) !=
                std::tolower(static_cast<unsigned char>(needle[i]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

}  // namespace lci
