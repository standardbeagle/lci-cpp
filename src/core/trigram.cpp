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
    int bucket_start, int bucket_end,
    SlabAllocator<FileLocation>* /*allocator*/) {
    int limit = std::min(bucket_end, static_cast<int>(result.buckets.size()));
    for (int bid = bucket_start; bid < limit; ++bid) {
        const auto& bucket_data = result.buckets[static_cast<size_t>(bid)];
        if (bucket_data.trigrams.empty()) continue;

        auto& bucket = buckets_[static_cast<size_t>(bid)];

        for (const auto& [trigram_hash, offsets] : bucket_data.trigrams) {
            auto& entry = bucket.trigrams[trigram_hash];

            size_t old_len = entry.locations.size();
            entry.locations.resize(old_len + offsets.size());

            for (size_t j = 0; j < offsets.size(); ++j) {
                entry.locations[old_len + j] = {result.file_id, offsets[j]};
            }
        }
    }
}

void ShardedTrigramStorage::merge_bucketed_trigrams(
    const BucketedTrigramResult& result,
    SlabAllocator<FileLocation>* allocator) {
    merge_bucket_data_for_worker(
        result, 0, static_cast<int>(result.buckets.size()), allocator);
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
      sharded_storage_(256) {}

void TrigramIndex::clear() {
    ascii_trigrams_.clear();
    unicode_trigrams_.clear();
    invalidated_files_.clear();
    search_cache_.clear();
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
    active_indexing_ops_.fetch_add(1, std::memory_order_relaxed);

    invalidated_files_.erase(file_id);

    if (is_pure_ascii(content)) {
        auto trigrams = extract_simple_trigrams(content);
        for (const auto& [offset, trigram_hash] : trigrams) {
            auto& entry = ascii_trigrams_[trigram_hash];
            entry.locations.push_back({file_id, static_cast<uint32_t>(offset)});
        }
    } else {
        auto trigrams = extract_unicode_trigrams(content);
        for (const auto& [offset, trigram_str] : trigrams) {
            auto& entry = unicode_trigrams_[trigram_str];
            entry.locations.push_back({file_id, static_cast<uint32_t>(offset)});
        }
    }

    active_indexing_ops_.fetch_sub(1, std::memory_order_relaxed);
}

void TrigramIndex::index_file_with_trigrams(
    FileID file_id,
    const absl::flat_hash_map<uint32_t, std::vector<uint32_t>>& trigrams) {
    invalidated_files_.erase(file_id);

    for (const auto& [trigram_hash, offsets] : trigrams) {
        if (offsets.empty()) continue;

        auto& entry = ascii_trigrams_[trigram_hash];
        size_t old_len = entry.locations.size();
        entry.locations.resize(old_len + offsets.size());

        for (size_t j = 0; j < offsets.size(); ++j) {
            entry.locations[old_len + j] = {file_id, offsets[j]};
        }
    }
}

void TrigramIndex::index_file_with_bucketed_trigrams(
    const BucketedTrigramResult& result) {
    invalidated_files_.erase(result.file_id);
    sharded_storage_.merge_bucketed_trigrams(result, &location_allocator_);
}

void TrigramIndex::remove_file(FileID file_id) {
    invalidated_files_.insert(file_id);

    if (static_cast<int>(invalidated_files_.size()) >= cleanup_threshold_) {
        perform_cleanup();
    }

    invalidate_cache_for_file(file_id);
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

    if (active_indexing_ops_.load(std::memory_order_relaxed) == 0) {
        auto cached = get_from_cache(search_pattern);
        if (!cached.empty()) return cached;
    }

    auto pattern_bytes = std::string_view(search_pattern);

    if (is_pure_ascii(pattern_bytes)) {
        auto all_pattern_trigrams = extract_simple_trigrams(pattern_bytes);
        int total_trigrams = static_cast<int>(all_pattern_trigrams.size());

        absl::flat_hash_map<FileID, int> file_trigram_counts;
        for (const auto& [_, trigram_hash] : all_pattern_trigrams) {
            auto it = ascii_trigrams_.find(trigram_hash);
            if (it != ascii_trigrams_.end()) {
                for (const auto& loc : it->second.locations) {
                    file_trigram_counts[loc.file_id]++;
                }
            }
        }

        return filter_and_return_candidates(
            file_trigram_counts, total_trigrams, search_pattern);
    }

    auto all_pattern_trigrams = extract_unicode_trigrams(pattern_bytes);
    int total_trigrams = static_cast<int>(all_pattern_trigrams.size());

    absl::flat_hash_map<FileID, int> file_trigram_counts;
    for (const auto& [_, trigram_str] : all_pattern_trigrams) {
        auto it = unicode_trigrams_.find(trigram_str);
        if (it != unicode_trigrams_.end()) {
            for (const auto& loc : it->second.locations) {
                file_trigram_counts[loc.file_id]++;
            }
        }
    }

    return filter_and_return_candidates(
        file_trigram_counts, total_trigrams, search_pattern);
}

int TrigramIndex::file_count() const {
    absl::flat_hash_set<FileID> unique_files;

    for (const auto& [_, entry] : ascii_trigrams_) {
        for (const auto& loc : entry.locations) {
            if (!invalidated_files_.contains(loc.file_id)) {
                unique_files.insert(loc.file_id);
            }
        }
    }

    for (const auto& [_, entry] : unicode_trigrams_) {
        for (const auto& loc : entry.locations) {
            if (!invalidated_files_.contains(loc.file_id)) {
                unique_files.insert(loc.file_id);
            }
        }
    }

    return static_cast<int>(unique_files.size());
}

int TrigramIndex::get_invalidation_count() const {
    return static_cast<int>(invalidated_files_.size());
}

void TrigramIndex::set_cleanup_threshold(int threshold) {
    cleanup_threshold_ = threshold;
}

void TrigramIndex::force_cleanup() {
    perform_cleanup();
}

void TrigramIndex::set_bulk_indexing(bool enabled) {
    bulk_indexing_.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

SlabAllocator<FileLocation>& TrigramIndex::get_allocator() {
    return location_allocator_;
}

ShardedTrigramStorage& TrigramIndex::sharded_storage() {
    return sharded_storage_;
}

void TrigramIndex::invalidate_cache_completely() {
    search_cache_.clear();
}

std::vector<FileID> TrigramIndex::get_from_cache(const std::string& pattern) const {
    auto it = search_cache_.find(pattern);
    if (it == search_cache_.end()) return {};

    auto elapsed = std::chrono::steady_clock::now() - it->second.timestamp;
    if (elapsed > search_cache_ttl_) {
        search_cache_.erase(it);
        return {};
    }

    return it->second.results;
}

void TrigramIndex::set_cache(
    const std::string& pattern,
    const std::vector<FileID>& results) const {
    if (search_cache_.size() > 1000) {
        int to_evict = static_cast<int>(search_cache_.size()) / 10;
        std::vector<std::string> keys_to_remove;
        keys_to_remove.reserve(static_cast<size_t>(to_evict));
        for (const auto& [key, _] : search_cache_) {
            if (to_evict <= 0) break;
            keys_to_remove.push_back(key);
            --to_evict;
        }
        for (const auto& key : keys_to_remove) {
            search_cache_.erase(key);
        }
    }

    search_cache_[pattern] = {results, std::chrono::steady_clock::now()};
}

void TrigramIndex::invalidate_cache_for_file(FileID /*file_id*/) {
    search_cache_.clear();
}

void TrigramIndex::perform_cleanup() {
    if (invalidated_files_.empty()) return;

    auto invalidated = std::move(invalidated_files_);
    invalidated_files_.clear();

    std::vector<uint32_t> ascii_keys_to_remove;
    for (auto& [key, entry] : ascii_trigrams_) {
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
        ascii_trigrams_.erase(key);
    }

    std::vector<std::string> unicode_keys_to_remove;
    for (auto& [key, entry] : unicode_trigrams_) {
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
        unicode_trigrams_.erase(key);
    }
}

std::vector<FileID> TrigramIndex::filter_and_return_candidates(
    const absl::flat_hash_map<FileID, int>& file_trigram_counts,
    int total_trigrams,
    const std::string& pattern) const {
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
            && !invalidated_files_.contains(file_id)) {
            candidates.push_back(file_id);
        }
    }

    if (!candidates.empty()
        && active_indexing_ops_.load(std::memory_order_relaxed) == 0) {
        set_cache(pattern, candidates);
    }

    return candidates;
}

}  // namespace lci
