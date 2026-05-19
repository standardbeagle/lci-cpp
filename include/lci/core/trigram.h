#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <lci/alloc/slab_allocator.h>
#include <lci/types.h>

namespace lci {

/// Location of a trigram occurrence within a file.
struct FileLocation {
    FileID file_id{};
    uint32_t offset{};
};

/// A collection of file locations sharing the same trigram.
struct TrigramEntry {
    std::vector<FileLocation> locations;
};

/// Cached search result with expiry timestamp.
struct SearchCacheEntry {
    std::vector<FileID> results;
    std::chrono::steady_clock::time_point timestamp;
};

/// A single shard (bucket) of the trigram index.
/// Each bucket holds its own trigram map, enabling parallel access
/// across different buckets without contention.
struct TrigramBucket {
    absl::flat_hash_map<uint32_t, TrigramEntry> trigrams;
};

/// Pre-bucketed trigram data from a single file for one bucket.
struct BucketedTrigramData {
    absl::flat_hash_map<uint32_t, std::vector<uint32_t>> trigrams;
};

/// Pre-bucketed trigram result for a single file across all buckets.
struct BucketedTrigramResult {
    FileID file_id{};
    std::vector<BucketedTrigramData> buckets;
};

/// 256-bucket sharded trigram storage for lock-free parallel merging.
///
/// Each bucket can be updated independently. During bulk indexing,
/// different worker threads process different bucket ranges without
/// contention.
class ShardedTrigramStorage {
  public:
    explicit ShardedTrigramStorage(uint16_t bucket_count = 256);

    /// Returns the bucket for a given trigram hash (read-only access).
    const TrigramBucket& get_bucket(uint32_t trigram_hash) const;

    /// Returns a mutable bucket by ID (for merge operations).
    TrigramBucket& get_bucket_by_id(int bucket_id);

    /// Returns the total number of buckets.
    int get_bucket_count() const;

    /// Merges pre-bucketed trigrams for a specific bucket range.
    /// Thread-safe when different threads process non-overlapping ranges.
    void merge_bucket_data_for_worker(
        const BucketedTrigramResult& result,
        int bucket_start, int bucket_end);

    /// Merges all buckets from a pre-bucketed result.
    void merge_bucketed_trigrams(const BucketedTrigramResult& result);

    /// Searches for a trigram across the appropriate bucket.
    std::vector<FileLocation> search_trigram(uint32_t trigram_hash) const;

    /// Removes all occurrences of a file from all buckets.
    void remove_file(FileID file_id);

    /// Removes all trigrams from all buckets.
    void clear();

  private:
    std::vector<TrigramBucket> buckets_;
    uint16_t bucket_count_;
    uint32_t bucket_mask_;
};

/// Trigram index supporting ASCII (bit-shifted uint32) and Unicode
/// (string-keyed) trigrams with 256-bucket sharded storage and
/// a 5-minute LRU search cache.
///
/// Architecture:
///   - ASCII trigrams: (b0 << 16) | (b1 << 8) | b2
///   - Unicode trigrams: 3-rune string keys
///   - 256 sharded buckets for lock-free parallel merging
///   - Search cache with configurable TTL (default 5 minutes)
///   - Lazy file invalidation with threshold-based cleanup
class TrigramIndex {
  public:
    TrigramIndex();

    /// Removes all trigrams and resets state.
    void clear();

    /// Estimates trigram count for pre-allocation.
    int predict_trigram_count(int content_size) const;

    /// Returns the bucket ID for a trigram hash.
    uint16_t get_bucket_for_trigram(uint32_t trigram_hash) const;

    /// Returns the total number of sharding buckets.
    int get_bucket_count() const;

    /// Creates a properly-sized bucketed result structure.
    BucketedTrigramResult create_bucketed_result(FileID file_id) const;

    /// Indexes a file directly from raw content.
    void index_file(FileID file_id, std::string_view content);

    /// Indexes a file using pre-computed trigram-to-offsets map.
    void index_file_with_trigrams(
        FileID file_id,
        const absl::flat_hash_map<uint32_t, std::vector<uint32_t>>& trigrams);

    /// Indexes a file using pre-bucketed trigrams.
    void index_file_with_bucketed_trigrams(const BucketedTrigramResult& result);

    /// Marks a file as invalidated (lazy removal).
    void remove_file(FileID file_id);

    /// Returns candidate file IDs matching a pattern's trigrams.
    std::vector<FileID> find_candidates(std::string_view pattern) const;

    /// Returns candidate file IDs with case-sensitivity option.
    std::vector<FileID> find_candidates_with_options(
        std::string_view pattern, bool case_insensitive) const;

    /// Returns the number of unique files in the index.
    int file_count() const;

    /// Returns current invalidation list size.
    int get_invalidation_count() const;

    /// Sets the threshold for triggering background cleanup.
    void set_cleanup_threshold(int threshold);

    /// Forces immediate cleanup of invalidated files.
    void force_cleanup();

    /// Sets bulk indexing mode (skips cache during indexing).
    void set_bulk_indexing(bool enabled);

    /// Returns the slab allocator for use by merge pipelines.
    SlabAllocator<FileLocation>& get_allocator();

    /// Returns the underlying sharded storage.
    ShardedTrigramStorage& sharded_storage();

    /// Invalidates the entire search cache.
    void invalidate_cache_completely();

  private:
    absl::flat_hash_map<uint32_t, TrigramEntry> ascii_trigrams_;
    absl::flat_hash_map<std::string, TrigramEntry> unicode_trigrams_;

    SlabAllocator<FileLocation> location_allocator_;

    absl::flat_hash_set<FileID> invalidated_files_;
    int cleanup_threshold_{100};

    mutable absl::flat_hash_map<std::string, SearchCacheEntry> search_cache_;
    std::chrono::seconds search_cache_ttl_{300};

    std::atomic<int32_t> active_indexing_ops_{0};
    std::atomic<int32_t> bulk_indexing_{0};

    uint16_t bucket_count_{256};
    uint32_t bucket_mask_{255};

    ShardedTrigramStorage sharded_storage_;

    /// Retrieves a cached search result if valid.
    std::vector<FileID> get_from_cache(const std::string& pattern) const;

    /// Stores a search result in the cache.
    void set_cache(const std::string& pattern,
                   const std::vector<FileID>& results) const;

    /// Invalidates cache entries that may reference a file.
    void invalidate_cache_for_file(FileID file_id);

    /// Performs cleanup of invalidated files from all trigram maps.
    void perform_cleanup();

    /// Filters candidate results by match count and invalidation status.
    std::vector<FileID> filter_and_return_candidates(
        const absl::flat_hash_map<FileID, int>& file_trigram_counts,
        int total_trigrams,
        const std::string& pattern) const;
};

// -- Free functions for trigram extraction ------------------------------------

/// Returns true if all bytes are ASCII (< 128).
bool is_pure_ascii(std::string_view content);

/// Returns true if a byte is alphanumeric or underscore.
inline bool is_alpha_num(uint8_t b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z')
           || (b >= '0' && b <= '9') || b == '_';
}

/// Extracts ASCII trigrams as (byte_offset -> uint32 hash) pairs.
absl::flat_hash_map<int, uint32_t> extract_simple_trigrams(
    std::string_view content);

/// Extracts Unicode trigrams as (byte_offset -> string) pairs.
absl::flat_hash_map<int, std::string> extract_unicode_trigrams(
    std::string_view content);

/// Distributes extracted trigrams into buckets for parallel merging.
BucketedTrigramResult bucket_trigrams(
    FileID file_id,
    const absl::flat_hash_map<int, uint32_t>& trigrams,
    int bucket_count);

}  // namespace lci
