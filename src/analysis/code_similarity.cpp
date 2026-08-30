#include <lci/analysis/code_similarity.h>

namespace lci {

namespace {

bool is_code_delimiter(char ch) {
    switch (ch) {
        case '(': case ')': case '{': case '}': case '[': case ']':
        case ';': case ',': case '.': case '<': case '>': case '+':
        case '-': case '*': case '/': case '=': case '!': case '&':
        case '|': case '^': case '~': case '?': case ':':
        case ' ': case '\t': case '\n': case '\r':
            return true;
        default:
            return false;
    }
}

}  // namespace

std::string normalize_code_content(std::string_view content) {
    std::string result;
    result.reserve(content.size());

    size_t i = 0;
    while (i < content.size()) {
        // Find line boundaries.
        auto nl = content.find('\n', i);
        auto line = (nl == std::string_view::npos)
                        ? content.substr(i)
                        : content.substr(i, nl - i);

        // Trim leading/trailing whitespace.
        auto ls = line.find_first_not_of(" \t\r");
        if (ls == std::string_view::npos) {
            i = (nl == std::string_view::npos) ? content.size() : nl + 1;
            continue;
        }
        auto trimmed = line.substr(ls);
        auto re = trimmed.find_last_not_of(" \t\r");
        if (re != std::string_view::npos) trimmed = trimmed.substr(0, re + 1);

        // Skip comment-only and blank lines.
        if (trimmed.empty() || trimmed.starts_with("//") ||
            trimmed.starts_with("#")) {
            i = (nl == std::string_view::npos) ? content.size() : nl + 1;
            continue;
        }

        if (!result.empty()) result += '\n';
        result.append(trimmed);
        i = (nl == std::string_view::npos) ? content.size() : nl + 1;
    }
    return result;
}

absl::flat_hash_set<std::string> code_token_set(std::string_view content) {
    absl::flat_hash_set<std::string> out;
    std::string current;
    for (char ch : content) {
        if (is_code_delimiter(ch)) {
            if (!current.empty()) {
                out.insert(std::move(current));
                current.clear();
            }
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                out.insert(std::string(1, ch));
            }
        } else {
            current += ch;
        }
    }
    if (!current.empty()) out.insert(std::move(current));
    return out;
}

double token_set_similarity(const absl::flat_hash_set<std::string>& a,
                            const absl::flat_hash_set<std::string>& b) {
    if (a.empty() || b.empty()) return 0.0;
    const auto& small = a.size() <= b.size() ? a : b;
    const auto& large = a.size() <= b.size() ? b : a;
    size_t intersection = 0;
    for (const auto& t : small) {
        if (large.contains(t)) ++intersection;
    }
    size_t union_size = a.size() + b.size() - intersection;
    if (union_size == 0) return 0.0;
    return static_cast<double>(intersection) / static_cast<double>(union_size);
}

double code_structural_similarity(std::string_view a, std::string_view b) {
    return token_set_similarity(code_token_set(a), code_token_set(b));
}

}  // namespace lci
