#include "diff_engine/canonicalize.h"

#include <algorithm>
#include <cstdio>
#include <regex>
#include <string>

namespace lci::parity {

namespace {

bool path_in(const std::vector<std::string>& patterns, const std::string& p) {
    for (const auto& q : patterns) if (q == p) return true;
    return false;
}

void strip_paths_recursive(nlohmann::json& node,
                           const std::vector<std::string>& patterns,
                           std::string current_path) {
    if (node.is_object()) {
        std::vector<std::string> to_remove;
        for (auto it = node.begin(); it != node.end(); ++it) {
            std::string child_path =
                current_path.empty() ? it.key() : current_path + "." + it.key();
            if (path_in(patterns, child_path)) {
                to_remove.push_back(it.key());
            } else {
                strip_paths_recursive(it.value(), patterns, child_path);
            }
        }
        for (const auto& k : to_remove) node.erase(k);
    } else if (node.is_array()) {
        std::string array_path = current_path + "[]";
        for (auto& elem : node) {
            strip_paths_recursive(elem, patterns, array_path);
        }
    }
}

void rewrite_paths_recursive(nlohmann::json& node,
                             const std::string& corpus_prefix) {
    if (corpus_prefix.empty()) return;
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            rewrite_paths_recursive(it.value(), corpus_prefix);
        }
    } else if (node.is_array()) {
        for (auto& elem : node) rewrite_paths_recursive(elem, corpus_prefix);
    } else if (node.is_string()) {
        std::string s = node.get<std::string>();
        if (s.size() >= corpus_prefix.size() &&
            s.compare(0, corpus_prefix.size(), corpus_prefix) == 0 &&
            (s.size() == corpus_prefix.size() || s[corpus_prefix.size()] == '/')) {
            node = std::string("${CORPUS}") + s.substr(corpus_prefix.size());
        }
    }
}

void normalize_floats_recursive(nlohmann::json& node,
                                const std::vector<std::string>& preserve,
                                std::string current_path) {
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            std::string child = current_path.empty()
                                ? it.key() : current_path + "." + it.key();
            normalize_floats_recursive(it.value(), preserve, child);
        }
    } else if (node.is_array()) {
        std::string array_path = current_path + "[]";
        for (auto& elem : node) {
            normalize_floats_recursive(elem, preserve, array_path);
        }
    } else if (node.is_number_float()) {
        // Skip preserved-number paths so ranked/timed tiers can apply
        // numeric tolerance later.
        if (path_in(preserve, current_path)) return;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6g", node.get<double>());
        node = std::string(buf);
    }
}

nlohmann::json sort_keys_recursive(const nlohmann::json& in) {
    if (in.is_object()) {
        // Walk keys in sorted order via nlohmann's ordered map view —
        // nlohmann::json already sorts keys when re-emitting via dump if we
        // construct a fresh object insertion-ordered with sorted keys.
        std::vector<std::string> keys;
        for (auto it = in.begin(); it != in.end(); ++it) keys.push_back(it.key());
        std::sort(keys.begin(), keys.end());
        nlohmann::json out = nlohmann::json::object();
        for (const auto& k : keys) {
            out[k] = sort_keys_recursive(in.at(k));
        }
        return out;
    }
    if (in.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& elem : in) out.push_back(sort_keys_recursive(elem));
        return out;
    }
    return in;
}

// ---------- text-mode helpers ----------

// Match "<digits>ms" or "<digits>.<digits>ms".  ECMAScript flavor.
const std::regex& timing_re() {
    static const std::regex re(R"(\d+(\.\d+)?ms)",
                               std::regex::ECMAScript | std::regex::optimize);
    return re;
}

void scrub_timing_inplace(std::string& line) {
    line = std::regex_replace(line, timing_re(), "<MS>");
}

void rewrite_corpus_prefix_inplace(std::string& line,
                                   const std::string& prefix) {
    if (prefix.empty() || line.empty()) return;
    std::string out;
    out.reserve(line.size());
    size_t i = 0;
    while (i < line.size()) {
        size_t hit = line.find(prefix, i);
        if (hit == std::string::npos) {
            out.append(line, i, std::string::npos);
            break;
        }
        out.append(line, i, hit - i);
        out.append("${CORPUS}");
        i = hit + prefix.size();
    }
    line = std::move(out);
}

// Strip a leading emoji (any char in the U+0080..U+10FFFF range that
// starts a multi-byte UTF-8 sequence) plus an optional variation selector
// (U+FE0F) plus the whitespace that follows.  Conservative: drops at most
// one emoji glyph per line.
void strip_emoji_prefix_inplace(std::string& line) {
    if (line.empty()) return;
    auto u = static_cast<unsigned char>(line[0]);
    if (u < 0x80) return;  // ASCII — nothing to strip.

    size_t i = 0;
    // Consume one UTF-8 codepoint.
    auto cp_len = [](unsigned char c) -> size_t {
        if ((c & 0x80) == 0x00) return 1;
        if ((c & 0xE0) == 0xC0) return 2;
        if ((c & 0xF0) == 0xE0) return 3;
        if ((c & 0xF8) == 0xF0) return 4;
        return 1;
    };
    size_t first = cp_len(u);
    if (first == 0 || first > line.size()) return;
    i += first;

    // Optionally consume the U+FE0F variation selector (UTF-8: EF B8 8F).
    if (i + 2 < line.size() &&
        static_cast<unsigned char>(line[i])     == 0xEF &&
        static_cast<unsigned char>(line[i + 1]) == 0xB8 &&
        static_cast<unsigned char>(line[i + 2]) == 0x8F) {
        i += 3;
    }

    // Require whitespace after the emoji — otherwise it's probably not a
    // prefix glyph, leave the line alone.
    if (i >= line.size() || (line[i] != ' ' && line[i] != '\t')) return;

    // Consume whitespace run.
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;

    line.erase(0, i);
}

bool line_has_strip_substring(const std::string& line,
                              const std::vector<std::string>& strips) {
    for (const auto& s : strips) {
        if (!s.empty() && line.find(s) != std::string::npos) return true;
    }
    return false;
}

void apply_replace_rules(std::string& line,
                         const std::vector<std::pair<std::string, std::string>>& rules) {
    for (const auto& [pat, rep] : rules) {
        // Compile per-call.  Acceptable: descriptors carry only a handful
        // of rules and parity tests run once per descriptor.
        std::regex re(pat, std::regex::ECMAScript | std::regex::optimize);
        line = std::regex_replace(line, re, rep);
    }
}

} // namespace

nlohmann::json canonicalize_json(const nlohmann::json& in,
                                 const CanonicalizeOptions& opts) {
    nlohmann::json out = in;
    strip_paths_recursive(out, opts.ignore_paths, "");
    rewrite_paths_recursive(out, opts.corpus_prefix);
    normalize_floats_recursive(out, opts.preserve_number_paths, "");
    out = sort_keys_recursive(out);
    return out;
}

std::string canonicalize_text(std::string_view in,
                              const TextCanonicalizeOptions& opts) {
    std::string out;
    out.reserve(in.size());
    size_t pos = 0;
    while (pos < in.size()) {
        size_t nl = in.find('\n', pos);
        std::string_view raw =
            (nl == std::string_view::npos) ? in.substr(pos)
                                           : in.substr(pos, nl - pos);

        // Trim trailing whitespace (space, tab, CR).
        size_t end = raw.size();
        while (end > 0 && (raw[end - 1] == ' ' || raw[end - 1] == '\t' ||
                           raw[end - 1] == '\r')) {
            --end;
        }
        std::string line(raw.substr(0, end));

        // strip_lines BEFORE other transforms so that strip patterns can
        // match the original DEBUG/banner text without worrying about
        // timing scrub or path rewrites mangling them first.
        if (line_has_strip_substring(line, opts.strip_lines)) {
            // Also drop the trailing newline that would have followed
            // this line, so we don't leave a blank line behind.
            if (nl == std::string_view::npos) break;
            pos = nl + 1;
            continue;
        }

        if (opts.strip_emoji_prefix) strip_emoji_prefix_inplace(line);
        if (opts.scrub_timing)       scrub_timing_inplace(line);
        rewrite_corpus_prefix_inplace(line, opts.corpus_prefix);
        if (!opts.replace.empty())   apply_replace_rules(line, opts.replace);

        out.append(line);
        if (nl == std::string_view::npos) break;
        out.push_back('\n');
        pos = nl + 1;
    }
    return out;
}

} // namespace lci::parity
