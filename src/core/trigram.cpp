#include <lci/core/trigram.h>

#include <algorithm>
#include <cstddef>
#include <string>

namespace lci {
namespace {

/// Converts a UTF-8 string_view to a vector of Unicode code points.
std::vector<uint32_t> to_code_points(std::string_view s) {
    std::vector<uint32_t> result;
    result.reserve(s.size());

    size_t i = 0;
    while (i < s.size()) {
        auto b0 = static_cast<uint8_t>(s[i]);
        uint32_t cp = 0;
        int len = 0;

        if (b0 < 0x80) {
            cp = b0;
            len = 1;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp = b0 & 0x1F;
            len = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0F;
            len = 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp = b0 & 0x07;
            len = 4;
        } else {
            ++i;
            continue;
        }

        if (i + static_cast<size_t>(len) > s.size()) break;

        for (int j = 1; j < len; ++j) {
            cp = (cp << 6) | (static_cast<uint8_t>(s[i + static_cast<size_t>(j)]) & 0x3F);
        }

        result.push_back(cp);
        i += static_cast<size_t>(len);
    }

    return result;
}

/// Returns the byte length of a single code point encoded in UTF-8.
int utf8_char_len(uint8_t first_byte) {
    if (first_byte < 0x80) return 1;
    if ((first_byte & 0xE0) == 0xC0) return 2;
    if ((first_byte & 0xF0) == 0xE0) return 3;
    if ((first_byte & 0xF8) == 0xF0) return 4;
    return 1;
}

/// Returns true if a Unicode code point is alphanumeric or underscore.
bool is_alpha_num_unicode(uint32_t cp) {
    if (cp == '_') return true;
    if (cp >= 'a' && cp <= 'z') return true;
    if (cp >= 'A' && cp <= 'Z') return true;
    if (cp >= '0' && cp <= '9') return true;
    // Accept letters/digits outside ASCII range.
    if (cp > 127) return true;
    return false;
}

/// Encodes a code point to UTF-8 and appends to the string.
void append_code_point(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/// Computes byte offsets for each code point in a UTF-8 string.
std::vector<size_t> compute_byte_offsets(std::string_view s) {
    std::vector<size_t> offsets;
    offsets.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        offsets.push_back(i);
        i += static_cast<size_t>(utf8_char_len(static_cast<uint8_t>(s[i])));
    }
    return offsets;
}

/// Lowercase ASCII character.
char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c + ('a' - 'A'));
    return c;
}

/// Returns a lowercased copy of ASCII content.
std::string to_lower_ascii(std::string_view s) {
    std::string result(s.size(), '\0');
    for (size_t i = 0; i < s.size(); ++i) {
        result[i] = ascii_lower(s[i]);
    }
    return result;
}

/// Removes locations for a specific file from a vector.
void remove_file_locations(std::vector<FileLocation>& locations, FileID file_id) {
    locations.erase(
        std::remove_if(locations.begin(), locations.end(),
                       [file_id](const FileLocation& loc) {
                           return loc.file_id == file_id;
                       }),
        locations.end());
}

}  // namespace

// -- Free functions -----------------------------------------------------------

bool is_pure_ascii(std::string_view content) {
    for (auto c : content) {
        if (static_cast<uint8_t>(c) > 127) return false;
    }
    return true;
}

absl::flat_hash_map<int, uint32_t> extract_simple_trigrams(
    std::string_view content) {
    absl::flat_hash_map<int, uint32_t> trigrams;
    if (content.size() < 3) return trigrams;

    for (size_t i = 0; i + 2 < content.size(); ++i) {
        auto b0 = static_cast<uint8_t>(content[i]);
        auto b1 = static_cast<uint8_t>(content[i + 1]);
        auto b2 = static_cast<uint8_t>(content[i + 2]);

        bool has_alpha = is_alpha_num(b0) || is_alpha_num(b1) || is_alpha_num(b2);
        if (!has_alpha) continue;

        uint32_t trigram = (uint32_t(b0) << 16) | (uint32_t(b1) << 8) | uint32_t(b2);
        trigrams[static_cast<int>(i)] = trigram;
    }

    return trigrams;
}

absl::flat_hash_map<int, std::string> extract_unicode_trigrams(
    std::string_view content) {
    absl::flat_hash_map<int, std::string> trigrams;
    if (content.size() < 3) return trigrams;

    auto code_points = to_code_points(content);
    if (code_points.size() < 3) return trigrams;

    auto byte_offsets = compute_byte_offsets(content);

    for (size_t i = 0; i + 2 < code_points.size(); ++i) {
        bool has_alpha = is_alpha_num_unicode(code_points[i])
                         || is_alpha_num_unicode(code_points[i + 1])
                         || is_alpha_num_unicode(code_points[i + 2]);
        if (!has_alpha) continue;

        std::string trigram_str;
        trigram_str.reserve(12);
        append_code_point(trigram_str, code_points[i]);
        append_code_point(trigram_str, code_points[i + 1]);
        append_code_point(trigram_str, code_points[i + 2]);

        int byte_offset = static_cast<int>(byte_offsets[i]);
        trigrams[byte_offset] = std::move(trigram_str);
    }

    return trigrams;
}

// -- ShardedTrigramStorage ----------------------------------------------------

ShardedTrigramStorage::ShardedTrigramStorage(uint16_t bucket_count)
    : bucket_count_(bucket_count),
      bucket_mask_(static_cast<uint32_t>(bucket_count) - 1) {
    buckets_.resize(bucket_count);
}

const TrigramBucket& ShardedTrigramStorage::get_bucket(uint32_t trigram_hash) const {
    return buckets_[trigram_hash & bucket_mask_];
}

TrigramBucket& ShardedTrigramStorage::get_bucket_by_id(int bucket_id) {
    return buckets_[static_cast<size_t>(bucket_id)];
}

int ShardedTrigramStorage::get_bucket_count() const {
    return static_cast<int>(bucket_count_);
}

void ShardedTrigramStorage::merge_bucket_data_for_worker(
    const BucketedTrigramResult& result,
    int bucket_start, int bucket_end) {
    int limit = std::min(bucket_end, static_cast<int>(result.buckets.size()));
    for (int bid = bucket_start; bid < limit; ++bid) {
        const auto& bucket_data = result.buckets[static_cast<size_t>(bid)];
        if (bucket_data.trigrams.empty()) continue;

        auto& bucket = buckets_[static_cast<size_t>(bid)];

        // Reserve destination hash map capacity for the incoming trigrams
        // to skip mid-loop rehashing. cheap upper bound: existing size +
        // incoming distinct count (some will collide but capacity is just
        // a hint).
        bucket.trigrams.reserve(bucket.trigrams.size() +
                                bucket_data.trigrams.size());

        for (const auto& [trigram_hash, offsets] : bucket_data.trigrams) {
            auto& entry = bucket.trigrams[trigram_hash];

            // Reserve before append to skip the geometric realloc that
            // resize() does when capacity < old + needed.
            size_t old_len = entry.locations.size();
            entry.locations.reserve(old_len + offsets.size());
            entry.locations.resize(old_len + offsets.size());

            for (size_t j = 0; j < offsets.size(); ++j) {
                entry.locations[old_len + j] = {result.file_id, offsets[j]};
            }
        }
    }
}

void ShardedTrigramStorage::merge_bucketed_trigrams(
    const BucketedTrigramResult& result) {
    merge_bucket_data_for_worker(
        result, 0, static_cast<int>(result.buckets.size()));
}

std::vector<FileLocation> ShardedTrigramStorage::search_trigram(
    uint32_t trigram_hash) const {
    const auto& bucket = get_bucket(trigram_hash);
    auto it = bucket.trigrams.find(trigram_hash);
    if (it == bucket.trigrams.end()) return {};

    return it->second.locations;
}

void ShardedTrigramStorage::remove_file(FileID file_id) {
    for (auto& bucket : buckets_) {
        std::vector<uint32_t> keys_to_remove;
        for (auto& [key, entry] : bucket.trigrams) {
            remove_file_locations(entry.locations, file_id);
            if (entry.locations.empty()) {
                keys_to_remove.push_back(key);
            }
        }
        for (auto key : keys_to_remove) {
            bucket.trigrams.erase(key);
        }
    }
}

void ShardedTrigramStorage::clear() {
    for (auto& bucket : buckets_) {
        bucket.trigrams.clear();
    }
}

// -- TrigramIndex -------------------------------------------------------------

TrigramIndex::TrigramIndex()
    : location_allocator_(kTrigramTierConfigs),
      sharded_storage_(256) {
    snapshot_.store(std::make_shared<const Snapshot>(),
                    std::memory_order_release);
}

std::shared_ptr<const TrigramIndex::Snapshot>
TrigramIndex::load_snapshot() const {
    return snapshot_.load(std::memory_order_acquire);
}

template <class Fn>
void TrigramIndex::mutate_snapshot(Fn&& fn) {
    std::lock_guard<std::mutex> lk(write_mu_);
    auto next = std::make_shared<Snapshot>(
        *snapshot_.load(std::memory_order_acquire));
    fn(*next);
    snapshot_.store(std::move(next), std::memory_order_release);
}

void TrigramIndex::clear() {
    mutate_snapshot([](Snapshot& snap) {
        snap.ascii_trigrams.clear();
        snap.unicode_trigrams.clear();
        snap.invalidated_files.clear();
    });
    sharded_storage_.clear();
    location_allocator_.reset_stats();
}

int TrigramIndex::predict_trigram_count(int content_size) const {
    if (content_size <= 0) return 0;

    int predicted = content_size / 2;
    if (predicted < 8) return 8;
    if (predicted > 1000) return 1000;
    return predicted;
}

uint16_t TrigramIndex::get_bucket_for_trigram(uint32_t trigram_hash) const {
    return static_cast<uint16_t>(trigram_hash & bucket_mask_);
}

int TrigramIndex::get_bucket_count() const {
    return static_cast<int>(bucket_count_);
}

BucketedTrigramResult TrigramIndex::create_bucketed_result(FileID file_id) const {
    BucketedTrigramResult result;
    result.file_id = file_id;
    result.buckets.resize(bucket_count_);
    return result;
}

void TrigramIndex::index_file(FileID file_id, std::string_view content) {
    if (is_pure_ascii(content)) {
        auto trigrams = extract_simple_trigrams(content);
        mutate_snapshot([&](Snapshot& snap) {
            snap.invalidated_files.erase(file_id);
            for (const auto& [offset, trigram_hash] : trigrams) {
                auto& entry = snap.ascii_trigrams[trigram_hash];
                entry.locations.push_back(
                    {file_id, static_cast<uint32_t>(offset)});
            }
        });
    } else {
        auto trigrams = extract_unicode_trigrams(content);
        mutate_snapshot([&](Snapshot& snap) {
            snap.invalidated_files.erase(file_id);
            for (const auto& [offset, trigram_str] : trigrams) {
                auto& entry = snap.unicode_trigrams[trigram_str];
                entry.locations.push_back(
                    {file_id, static_cast<uint32_t>(offset)});
            }
        });
    }
}

void TrigramIndex::index_file_with_trigrams(
    FileID file_id,
    const absl::flat_hash_map<uint32_t, std::vector<uint32_t>>& trigrams) {
    mutate_snapshot([&](Snapshot& snap) {
        snap.invalidated_files.erase(file_id);
        for (const auto& [trigram_hash, offsets] : trigrams) {
            if (offsets.empty()) continue;

            auto& entry = snap.ascii_trigrams[trigram_hash];
            size_t old_len = entry.locations.size();
            entry.locations.resize(old_len + offsets.size());

            for (size_t j = 0; j < offsets.size(); ++j) {
                entry.locations[old_len + j] = {file_id, offsets[j]};
            }
        }
    });
}

void TrigramIndex::index_file_with_bucketed_trigrams(
    const BucketedTrigramResult& result) {
    // sharded_storage_ is outside the snapshot (bulk write-only, no search
    // reader); only the invalidation set is part of the read snapshot. On
    // the bulk path the invalidation set is normally empty, so skip the COW
    // entirely unless this file is actually pending invalidation — avoids a
    // per-file snapshot clone across a full reindex.
    if (load_snapshot()->invalidated_files.contains(result.file_id)) {
        mutate_snapshot([&](Snapshot& snap) {
            snap.invalidated_files.erase(result.file_id);
        });
    }
    sharded_storage_.merge_bucketed_trigrams(result);
}

void TrigramIndex::remove_file(FileID file_id) {
    mutate_snapshot([&](Snapshot& snap) {
        snap.invalidated_files.insert(file_id);
        if (static_cast<int>(snap.invalidated_files.size()) >=
            cleanup_threshold_) {
            cleanup_snapshot(snap);
        }
    });
}

std::vector<FileID> TrigramIndex::find_candidates(std::string_view pattern) const {
    return find_candidates_with_options(pattern, false);
}

std::vector<FileID> TrigramIndex::find_candidates_with_options(
    std::string_view pattern, bool case_insensitive) const {
    if (pattern.size() < 3) return {};

    std::string search_pattern(pattern);
    if (case_insensitive) {
        search_pattern = to_lower_ascii(pattern);
    }

    // Lock-free read: load the immutable snapshot once and operate on it.
    // The shared_ptr keeps it alive even if a writer publishes a new one
    // mid-read. (The former per-query result cache was removed: it mutated
    // shared state on the read path under only a shared lock — a data race —
    // and benchmarks showed candidate lookup is already faster without it.)
    auto snap = load_snapshot();

    auto pattern_bytes = std::string_view(search_pattern);

    if (is_pure_ascii(pattern_bytes)) {
        auto all_pattern_trigrams = extract_simple_trigrams(pattern_bytes);
        int total_trigrams = static_cast<int>(all_pattern_trigrams.size());

        absl::flat_hash_map<FileID, int> file_trigram_counts;
        for (const auto& [_, trigram_hash] : all_pattern_trigrams) {
            auto it = snap->ascii_trigrams.find(trigram_hash);
            if (it != snap->ascii_trigrams.end()) {
                for (const auto& loc : it->second.locations) {
                    file_trigram_counts[loc.file_id]++;
                }
            }
        }

        return filter_and_return_candidates(
            *snap, file_trigram_counts, total_trigrams);
    }

    auto all_pattern_trigrams = extract_unicode_trigrams(pattern_bytes);
    int total_trigrams = static_cast<int>(all_pattern_trigrams.size());

    absl::flat_hash_map<FileID, int> file_trigram_counts;
    for (const auto& [_, trigram_str] : all_pattern_trigrams) {
        auto it = snap->unicode_trigrams.find(trigram_str);
        if (it != snap->unicode_trigrams.end()) {
            for (const auto& loc : it->second.locations) {
                file_trigram_counts[loc.file_id]++;
            }
        }
    }

    return filter_and_return_candidates(
        *snap, file_trigram_counts, total_trigrams);
}

int TrigramIndex::file_count() const {
    auto snap = load_snapshot();
    absl::flat_hash_set<FileID> unique_files;

    for (const auto& [_, entry] : snap->ascii_trigrams) {
        for (const auto& loc : entry.locations) {
            if (!snap->invalidated_files.contains(loc.file_id)) {
                unique_files.insert(loc.file_id);
            }
        }
    }

    for (const auto& [_, entry] : snap->unicode_trigrams) {
        for (const auto& loc : entry.locations) {
            if (!snap->invalidated_files.contains(loc.file_id)) {
                unique_files.insert(loc.file_id);
            }
        }
    }

    return static_cast<int>(unique_files.size());
}

int TrigramIndex::get_invalidation_count() const {
    return static_cast<int>(load_snapshot()->invalidated_files.size());
}

void TrigramIndex::set_cleanup_threshold(int threshold) {
    cleanup_threshold_ = threshold;
}

void TrigramIndex::force_cleanup() {
    mutate_snapshot([](Snapshot& snap) { cleanup_snapshot(snap); });
}

void TrigramIndex::set_bulk_indexing(bool /*enabled*/) {
    // No-op: TrigramIndex needs no bulk-build window. The read-path maps
    // (ascii/unicode) are written only by the incremental index_file path
    // and stay empty during a bulk reindex, which fills sharded_storage_
    // (outside the snapshot). Kept for interface symmetry with the other
    // indexes (MasterIndex::set_bulk_indexing fans out to all three).
}

SlabAllocator<FileLocation>& TrigramIndex::get_allocator() {
    return location_allocator_;
}

ShardedTrigramStorage& TrigramIndex::sharded_storage() {
    return sharded_storage_;
}

void TrigramIndex::cleanup_snapshot(Snapshot& snap) {
    if (snap.invalidated_files.empty()) return;

    auto invalidated = std::move(snap.invalidated_files);
    snap.invalidated_files.clear();

    std::vector<uint32_t> ascii_keys_to_remove;
    for (auto& [key, entry] : snap.ascii_trigrams) {
        auto& locs = entry.locations;
        locs.erase(
            std::remove_if(locs.begin(), locs.end(),
                           [&invalidated](const FileLocation& loc) {
                               return invalidated.contains(loc.file_id);
                           }),
            locs.end());
        if (locs.empty()) {
            ascii_keys_to_remove.push_back(key);
        }
    }
    for (auto key : ascii_keys_to_remove) {
        snap.ascii_trigrams.erase(key);
    }

    std::vector<std::string> unicode_keys_to_remove;
    for (auto& [key, entry] : snap.unicode_trigrams) {
        auto& locs = entry.locations;
        locs.erase(
            std::remove_if(locs.begin(), locs.end(),
                           [&invalidated](const FileLocation& loc) {
                               return invalidated.contains(loc.file_id);
                           }),
            locs.end());
        if (locs.empty()) {
            unicode_keys_to_remove.push_back(key);
        }
    }
    for (const auto& key : unicode_keys_to_remove) {
        snap.unicode_trigrams.erase(key);
    }
}

std::vector<FileID> TrigramIndex::filter_and_return_candidates(
    const Snapshot& snap,
    const absl::flat_hash_map<FileID, int>& file_trigram_counts,
    int total_trigrams) const {
    if (total_trigrams == 0) return {};

    int min_required = 1;
    if (total_trigrams > 6) {
        min_required = total_trigrams / 2;
    } else if (total_trigrams > 3) {
        min_required = 3;
    }

    std::vector<FileID> candidates;
    for (const auto& [file_id, match_count] : file_trigram_counts) {
        if (match_count >= min_required
            && !snap.invalidated_files.contains(file_id)) {
            candidates.push_back(file_id);
        }
    }

    return candidates;
}

}  // namespace lci
