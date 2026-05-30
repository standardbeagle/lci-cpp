#include <lci/core/reference_tracker.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace lci {

// ---------------------------------------------------------------------------
// PostingsIndex
// ---------------------------------------------------------------------------

PostingsIndex::PostingsIndex() {
    snapshot_.store(std::make_shared<const Snapshot>(),
                    std::memory_order_release);
}

std::shared_ptr<const PostingsIndex::Snapshot>
PostingsIndex::load_snapshot() const {
    return snapshot_.load(std::memory_order_acquire);
}

template <class Fn>
void PostingsIndex::write_snapshot(Fn&& fn) {
    std::lock_guard<std::mutex> lk(write_mu_);
    if (staging_) {
        // Bulk window: mutate the private unpublished snapshot in place;
        // a single publish happens in set_bulk_indexing(false).
        fn(*staging_);
        return;
    }
    auto next = std::make_shared<Snapshot>(
        *snapshot_.load(std::memory_order_acquire));
    fn(*next);
    snapshot_.store(std::move(next), std::memory_order_release);
}

void PostingsIndex::set_bulk_indexing(bool enabled) {
    std::lock_guard<std::mutex> lk(write_mu_);
    if (enabled) {
        if (!staging_) {
            staging_ = std::make_shared<Snapshot>(
                *snapshot_.load(std::memory_order_acquire));
        }
    } else if (staging_) {
        snapshot_.store(std::move(staging_), std::memory_order_release);
        staging_ = nullptr;
    }
}

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
                               std::string_view raw, int abs_start) {
    if (raw.size() < 3) return;

    // Reject non-ASCII without allocating — raw is a view into source.
    if (!is_all_ascii(raw)) return;

    // Thread-local lowercase buffer reused across tokens. Avoids the
    // previous per-call std::string alloc that fired ~once per token
    // and dominated PostingsIndex::add_token in malloc profiles.
    thread_local std::string lower;
    lower.assign(raw.size(), '\0');
    for (size_t i = 0; i < raw.size(); ++i) {
        lower[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(raw[i])));
    }

    // Trim leading/trailing non-alnum/underscore via string_view (zero
    // alloc).
    auto is_valid = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    };
    std::string_view trimmed(lower);
    while (!trimmed.empty() && !is_valid(trimmed.front())) {
        trimmed.remove_prefix(1);
    }
    while (!trimmed.empty() && !is_valid(trimmed.back())) {
        trimmed.remove_suffix(1);
    }

    if (trimmed.size() < 3) return;

    // try_emplace does the lookup + insert in one hash op (vs old code's
    // contains() + operator[] = two ops). Materialise the std::string
    // key only on insert; reusing the trimmed view via the heterogeneous
    // lookup overload of flat_hash_map.find lets us skip allocation on
    // duplicate hits.
    auto it = dst.find(trimmed);
    if (it == dst.end()) {
        dst.emplace(std::string(trimmed), abs_start);
    }
}

std::vector<PostingsToken> PostingsIndex::tokenize_content(
    std::string_view content) {
    std::vector<PostingsToken> result;
    if (content.empty()) return result;

    // Local dedup map: thread-local could leak state across files; a
    // per-call map is the safe default. Reserved heuristically based on
    // a rough 1-token-per-10-bytes density.
    absl::flat_hash_map<std::string, int> tokens_for_file;
    tokens_for_file.reserve(content.size() / 10 + 8);

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

    result.reserve(tokens_for_file.size());
    for (auto& [tok, off] : tokens_for_file) {
        result.push_back(PostingsToken{std::move(const_cast<std::string&>(tok)),
                                       off});
    }
    return result;
}

void PostingsIndex::index_file_pretokenized(FileID file_id,
                                            std::vector<PostingsToken> tokens) {
    if (tokens.empty()) return;

    write_snapshot([&](Snapshot& snap) {
        std::vector<std::string> keys;
        keys.reserve(tokens.size());
        for (const auto& pt : tokens) {
            keys.push_back(pt.token);
        }
        snap.reverse_keys[file_id] = std::move(keys);

        for (auto& pt : tokens) {
            auto& m = snap.tokens[pt.token];
            if (!m.contains(file_id)) {
                m[file_id] = pt.offset;
            }
        }
    });
}

void PostingsIndex::index_file(FileID file_id, std::string_view content) {
    if (content.empty()) return;
    index_file_pretokenized(file_id, tokenize_content(content));
}

void PostingsIndex::remove_file(FileID file_id) {
    write_snapshot([&](Snapshot& snap) {
        auto rk_it = snap.reverse_keys.find(file_id);
        if (rk_it == snap.reverse_keys.end()) return;

        for (const auto& tok : rk_it->second) {
            auto tok_it = snap.tokens.find(tok);
            if (tok_it != snap.tokens.end()) {
                tok_it->second.erase(file_id);
                if (tok_it->second.empty()) {
                    snap.tokens.erase(tok_it);
                }
            }
        }
        snap.reverse_keys.erase(rk_it);
    });
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

    // Lock-free read over the immutable snapshot.
    auto snap = load_snapshot();
    auto it = snap->tokens.find(tok);
    if (it == snap->tokens.end() || it->second.empty()) return;

    files_out.reserve(it->second.size());
    for (const auto& [fid, off] : it->second) {
        files_out.push_back(fid);
        offsets_out[fid] = off;
    }
}

int PostingsIndex::token_count() const {
    return static_cast<int>(load_snapshot()->tokens.size());
}

int PostingsIndex::file_count() const {
    return static_cast<int>(load_snapshot()->reverse_keys.size());
}

void PostingsIndex::clear() {
    write_snapshot([](Snapshot& snap) {
        snap.tokens.clear();
        snap.reverse_keys.clear();
    });
}

}  // namespace lci
