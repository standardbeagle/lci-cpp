#include <lci/core/reference_tracker.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace lci {

// ---------------------------------------------------------------------------
// PostingsIndex
// ---------------------------------------------------------------------------

bool PostingsIndex::is_token_char(uint8_t b) {
    return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') ||
           (b >= '0' && b <= '9') || b == '_';
}

bool PostingsIndex::is_all_ascii(std::string_view s) {
    for (char c : s) {
        if (static_cast<uint8_t>(c) > 0x7F) return false;
    }
    return true;
}

void PostingsIndex::add_token(absl::flat_hash_map<std::string, int>& dst,
                               std::string_view raw, int abs_start) const {
    if (raw.size() < 3) return;

    // Lowercase the token.
    std::string lower;
    lower.reserve(raw.size());
    for (char c : raw) {
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }

    if (!is_all_ascii(lower)) return;

    // Trim leading/trailing non-alnum/underscore.
    auto is_valid = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    };
    size_t start = 0;
    while (start < lower.size() && !is_valid(lower[start])) start++;
    size_t end = lower.size();
    while (end > start && !is_valid(lower[end - 1])) end--;

    if (end - start < 3) return;

    std::string tok = lower.substr(start, end - start);
    if (!dst.contains(tok)) {
        dst[tok] = abs_start;
    }
}

void PostingsIndex::index_file(FileID file_id, std::string_view content) {
    if (content.empty()) return;

    absl::flat_hash_map<std::string, int> tokens_for_file;
    int start = -1;

    for (int i = 0; i < static_cast<int>(content.size()); ++i) {
        auto b = static_cast<uint8_t>(content[static_cast<size_t>(i)]);
        if (is_token_char(b)) {
            if (start < 0) start = i;
            continue;
        }
        if (start >= 0) {
            add_token(tokens_for_file,
                      content.substr(static_cast<size_t>(start),
                                     static_cast<size_t>(i - start)),
                      start);
            start = -1;
        }
    }
    if (start >= 0) {
        add_token(tokens_for_file,
                  content.substr(static_cast<size_t>(start)),
                  start);
    }

    if (tokens_for_file.empty()) return;

    // Record reverse mapping for efficient removal.
    std::vector<std::string> keys;
    keys.reserve(tokens_for_file.size());
    for (const auto& [tok, _] : tokens_for_file) {
        keys.push_back(tok);
    }
    reverse_keys_[file_id] = std::move(keys);

    for (const auto& [tok, off] : tokens_for_file) {
        auto& m = tokens_[tok];
        if (!m.contains(file_id)) {
            m[file_id] = off;
        }
    }
}

void PostingsIndex::remove_file(FileID file_id) {
    auto rk_it = reverse_keys_.find(file_id);
    if (rk_it == reverse_keys_.end()) return;

    for (const auto& tok : rk_it->second) {
        auto tok_it = tokens_.find(tok);
        if (tok_it != tokens_.end()) {
            tok_it->second.erase(file_id);
            if (tok_it->second.empty()) {
                tokens_.erase(tok_it);
            }
        }
    }
    reverse_keys_.erase(rk_it);
}

void PostingsIndex::find(std::string_view token, bool case_insensitive,
                          std::vector<FileID>& files_out,
                          absl::flat_hash_map<FileID, int>& offsets_out) const {
    files_out.clear();
    offsets_out.clear();

    if (token.size() < 3) return;

    std::string tok;
    if (case_insensitive) {
        tok.reserve(token.size());
        for (char c : token) {
            tok.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        }
    } else {
        tok = std::string(token);
    }

    auto it = tokens_.find(tok);
    if (it == tokens_.end() || it->second.empty()) return;

    files_out.reserve(it->second.size());
    for (const auto& [fid, off] : it->second) {
        files_out.push_back(fid);
        offsets_out[fid] = off;
    }
}

int PostingsIndex::token_count() const {
    return static_cast<int>(tokens_.size());
}

int PostingsIndex::file_count() const {
    return static_cast<int>(reverse_keys_.size());
}

void PostingsIndex::clear() {
    tokens_.clear();
    reverse_keys_.clear();
}

}  // namespace lci
