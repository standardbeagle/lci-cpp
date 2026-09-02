#include <lci/parser/svelte_script.h>

#include <cctype>

namespace lci::parser {
namespace {

char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Case-insensitive search for `needle` (already lowercase) in `hay`
/// starting at `from`. Returns npos when absent. Handwritten scan: no
/// std::regex on the indexing path (karpathy rule), and needles are tiny.
std::size_t ifind(std::string_view hay, std::string_view needle,
                  std::size_t from) {
    if (needle.empty() || hay.size() < needle.size()) {
        return std::string_view::npos;
    }
    for (std::size_t i = from; i + needle.size() <= hay.size(); ++i) {
        std::size_t j = 0;
        while (j < needle.size() && lower(hay[i + j]) == needle[j]) ++j;
        if (j == needle.size()) return i;
    }
    return std::string_view::npos;
}

/// Finds the '>' closing an open tag starting at `tag_start` ('<'),
/// skipping quoted attribute values so `on:x="a > b"` cannot end the tag
/// early. Returns npos for an unterminated tag.
std::size_t find_tag_end(std::string_view s, std::size_t tag_start) {
    char quote = 0;
    for (std::size_t i = tag_start; i < s.size(); ++i) {
        char c = s[i];
        if (quote != 0) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            return i;
        }
    }
    return std::string_view::npos;
}

/// True when the open tag text (between "<script" and '>') declares a
/// TypeScript script: lang="ts" or lang="typescript" (either quote style).
bool open_tag_declares_typescript(std::string_view tag) {
    std::size_t lang = ifind(tag, "lang", 0);
    while (lang != std::string_view::npos) {
        std::size_t i = lang + 4;
        while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
        if (i < tag.size() && tag[i] == '=') {
            ++i;
            while (i < tag.size() &&
                   std::isspace(static_cast<unsigned char>(tag[i]))) {
                ++i;
            }
            if (i < tag.size() && (tag[i] == '"' || tag[i] == '\'')) ++i;
            std::string_view rest = tag.substr(i);
            if (ifind(rest, "typescript", 0) == 0) return true;
            if (rest.size() >= 2 && lower(rest[0]) == 't' &&
                lower(rest[1]) == 's' &&
                (rest.size() == 2 ||
                 !std::isalnum(static_cast<unsigned char>(rest[2])))) {
                return true;
            }
        }
        lang = ifind(tag, "lang", lang + 1);
    }
    return false;
}

}  // namespace

SvelteScriptInfo mask_svelte_script(std::string_view content,
                                    std::string& masked) {
    // Start fully blanked (newlines preserved), then copy script bodies in.
    masked.assign(content.size(), ' ');
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n' || content[i] == '\r') masked[i] = content[i];
    }

    SvelteScriptInfo info;
    std::size_t pos = 0;
    while (pos < content.size()) {
        std::size_t open = ifind(content, "<script", pos);
        if (open == std::string_view::npos) break;
        // Require a real tag boundary: "<scripts>" is not a script tag.
        std::size_t after = open + 7;
        if (after < content.size() && content[after] != '>' &&
            !std::isspace(static_cast<unsigned char>(content[after]))) {
            pos = open + 1;
            continue;
        }
        std::size_t tag_end = find_tag_end(content, open);
        if (tag_end == std::string_view::npos) break;  // unterminated tag

        std::string_view open_tag =
            content.substr(after, tag_end - after);
        std::size_t body_start = tag_end + 1;
        std::size_t close = ifind(content, "</script", body_start);
        if (close == std::string_view::npos) break;  // unterminated block

        info.has_script = true;
        if (open_tag_declares_typescript(open_tag)) info.typescript = true;
        for (std::size_t i = body_start; i < close; ++i) {
            masked[i] = content[i];
        }
        pos = close + 8;
    }
    return info;
}

}  // namespace lci::parser
