#include "diff_engine/canonicalize.h"

#include <cstdio>
#include <sstream>

namespace lci::parity {

namespace {

// Returns true if `path` matches any of the given JSONPath-lite expressions.
// Patterns supported:
//   "field"               — exact top-level field name
//   "a.b"                 — nested field
//   "results[].file"      — array element field (matches any index)
bool path_matches(const std::vector<std::string>& patterns,
                  const std::string& path) {
    for (const auto& p : patterns) {
        if (p == path) return true;
    }
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
            if (path_matches(patterns, child_path)) {
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
            s.compare(0, corpus_prefix.size(), corpus_prefix) == 0) {
            node = std::string("${CORPUS}") + s.substr(corpus_prefix.size());
        }
    }
}

bool path_in(const std::vector<std::string>& patterns, const std::string& p) {
    for (const auto& q : patterns) if (q == p) return true;
    return false;
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

std::string canonicalize_text(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    size_t pos = 0;
    while (pos < in.size()) {
        size_t nl = in.find('\n', pos);
        std::string_view line =
            (nl == std::string_view::npos) ? in.substr(pos)
                                           : in.substr(pos, nl - pos);
        // trim trailing whitespace (space, tab, CR)
        size_t end = line.size();
        while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t' ||
                           line[end - 1] == '\r')) {
            --end;
        }
        out.append(line.substr(0, end));
        if (nl == std::string_view::npos) break;
        out.push_back('\n');
        pos = nl + 1;
    }
    return out;
}

} // namespace lci::parity
