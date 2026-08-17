#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <lci/alloc/slab_allocator.h>
#include <lci/core/atomic_shared_ptr.h>
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
/// Per-file bloom filter over the file's trigram set — the file-granular
/// bulk trigram prefilter (the tracked follow-up from the 2026-08-04
/// per-occurrence removal). ~1 byte per DISTINCT trigram instead of ~12
/// bytes per occurrence, so a whole corpus costs megabytes, not gigabytes.
///
/// Contract: a strict superset test. may_contain() can false-positive
/// (the verify scan discards those) but never false-negatives, so
/// "some pattern trigram is absent" certifies the file pattern-free.
/// Build and query sides share the canonical hash + the extraction
/// window rule (windows with no alnum byte are skipped on BOTH sides).
class TrigramBloom {
  public:
    /// Builds from every trigram window of `content`. Never null; a
    /// content with zero qualifying windows yields a minimal bloom that
    /// correctly certifies every probe absent.
    static std::shared_ptr<const TrigramBloom> build(std::string_view content);

    bool may_contain(uint64_t canonical_hash) const {
        const uint32_t h1 = static_cast<uint32_t>(canonical_hash);
        const uint32_t h2 =
            static_cast<uint32_t>(canonical_hash >> 32) | 1u;
        for (uint32_t i = 0; i < kProbes; ++i) {
            const uint32_t bit = (h1 + i * h2) & bit_mask_;
            if ((bits_[bit >> 6] & (uint64_t{1} << (bit & 63))) == 0) {
                return false;
            }
        }
        return true;
    }

    size_t byte_size() const { return bits_.size() * sizeof(uint64_t); }

    /// Canonical hash of an ASCII trigram in packed (b0<<16|b1<<8|b2) form.
    static uint64_t hash_ascii(uint32_t packed);
    /// Canonical hash of a trigram's UTF-8 bytes. A pure-ASCII 3-byte
    /// trigram hashes identically to hash_ascii of its packed form, so a
    /// non-ASCII file's ASCII windows match ASCII pattern probes.
    static uint64_t hash_bytes(std::string_view trigram);

  private:
    static constexpr uint32_t kProbes = 4;
    std::vector<uint64_t> bits_;
    uint32_t bit_mask_{};  // nbits - 1 (nbits is a power of two)
};

class TrigramIndex {
  private:
    struct Snapshot;  // Defined in the private section below.

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

    /// Indexes a file directly from raw content. Trigram-hostile content
    /// (see is_trigram_hostile) is not trigram-indexed; the file is added
    /// to the unfiltered set instead so candidate search still returns it.
    void index_file(FileID file_id, std::string_view content);

    /// Registers a file as indexed without trigram data. Unfiltered files
    /// are always candidates — the fallback for content the trigram filter
    /// handles badly (per-occurrence storage would balloon, and near-random
    /// trigram sets defeat prefiltering anyway).
    void mark_unfiltered(FileID file_id);

    /// Indexes a file using pre-computed trigram-to-offsets map.
    void index_file_with_trigrams(
        FileID file_id,
        const absl::flat_hash_map<uint32_t, std::vector<uint32_t>>& trigrams);

    /// Indexes a file using pre-bucketed trigrams.
    void index_file_with_bucketed_trigrams(const BucketedTrigramResult& result);

    /// Installs a file's trigram bloom (bulk-pipeline feed; the worker
    /// builds it in parallel, the integrator installs it here). Clears any
    /// invalidated/unfiltered state for the file. Null is ignored.
    void set_file_bloom(FileID file_id,
                        std::shared_ptr<const TrigramBloom> bloom);

    /// Total bytes held by per-file blooms (index_size_bytes census).
    size_t bloom_bytes() const;

    /// Marks a file as invalidated (lazy removal).
    void remove_file(FileID file_id);

    /// Returns candidate file IDs matching a pattern's trigrams.
    std::vector<FileID> find_candidates(std::string_view pattern) const;

    /// Returns candidate file IDs with case-sensitivity option.
    std::vector<FileID> find_candidates_with_options(
        std::string_view pattern, bool case_insensitive) const;

    /// Certified-absence narrowing scoped to the files this index COVERS.
    ///
    /// The snapshot maps are populated only by the incremental index_file
    /// path (the bulk pipeline stopped producing trigram data, 2026-08-04),
    /// so at any moment they describe an arbitrary SUBSET of the corpus.
    /// Narrowing on them is sound only per covered file: a file absent
    /// from `possible` is certified pattern-free ONLY when `covered_files`
    /// contains it. Uncovered files (bulk-indexed, unfiltered/hostile)
    /// must always be scanned.
    ///
    /// `informative` is false — nothing is certified — when the pattern is
    /// shorter than 3 bytes, or the query is case-insensitive (stored
    /// trigrams keep original case, so a lowercased query would produce
    /// false absences).
    ///
    /// ASCII patterns probe BOTH trigram maps: non-ASCII files index all
    /// their trigrams (including pure-ASCII windows) in the unicode map
    /// only, so an ascii-map-only probe would falsely certify absence in
    /// covered non-ASCII files.
    class Narrowing {
      public:
        bool informative() const { return informative_; }
        /// True when this index certifies `fid` cannot contain the pattern.
        bool certifies_absent(FileID fid) const;

      private:
        friend class TrigramIndex;
        bool informative_{false};
        absl::flat_hash_set<FileID> possible_;
        std::shared_ptr<const Snapshot> snap_;
        /// Canonical pattern-trigram hashes probed against per-file
        /// blooms (capped; probing fewer only widens the superset).
        std::vector<uint64_t> probes_;
    };
    Narrowing narrow(std::string_view pattern, bool case_insensitive) const;

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

  private:
    /// Immutable read-side state, swapped atomically (RCU). Readers load
    /// the shared_ptr once and operate on the frozen snapshot with zero
    /// locks; writers clone-mutate-publish under write_mu_. Mirrors
    /// FileContentStore's snapshot model.
    ///
    /// sharded_storage_ is intentionally NOT part of the snapshot: it is
    /// written only by the bulk merge path (index_file_with_bucketed_trigrams
    /// / the merger pipeline) and never read by the search path
    /// (search_trigram has no callers), so it carries no read-vs-write race
    /// and needs no snapshot. ascii_trigrams / unicode_trigrams are written
    /// only by the incremental index_file path, so cloning them on each
    /// incremental write is cheap (they stay empty during bulk indexing).
    struct Snapshot {
        absl::flat_hash_map<uint32_t, TrigramEntry> ascii_trigrams;
        absl::flat_hash_map<std::string, TrigramEntry> unicode_trigrams;
        absl::flat_hash_set<FileID> invalidated_files;
        /// Files indexed WITHOUT trigram data (trigram-hostile content:
        /// minified bundles, high-entropy data). They are unconditionally
        /// returned as candidates so the prefilter never hides them; the
        /// downstream verify scan does the real matching.
        absl::flat_hash_set<FileID> unfiltered_files;
        /// Files whose trigram data lives in THESE maps (incremental
        /// index_file / index_file_with_trigrams). narrow() may certify
        /// pattern absence only inside this set — bulk-indexed files never
        /// enter it and must always be scanned.
        absl::flat_hash_set<FileID> covered_files;
        /// Per-file trigram blooms (bulk pipeline AND incremental path).
        /// shared_ptr values keep the RCU clone at pointer cost.
        absl::flat_hash_map<FileID, std::shared_ptr<const TrigramBloom>>
            blooms;
    };

    AtomicSharedPtr<const Snapshot> snapshot_;
    mutable std::mutex write_mu_;
    /// Non-null only inside a bulk window (guarded by write_mu_): writes
    /// mutate this staging snapshot in place and one atomic publish
    /// happens on close, avoiding a per-file snapshot clone across a
    /// bulk reindex (same shape as PostingsIndex's bulk window).
    std::shared_ptr<Snapshot> staging_;

    SlabAllocator<FileLocation> location_allocator_;

    int cleanup_threshold_{100};

    uint16_t bucket_count_{256};
    uint32_t bucket_mask_{255};

    ShardedTrigramStorage sharded_storage_;

    /// Loads the current published read snapshot (lock-free).
    std::shared_ptr<const Snapshot> load_snapshot() const;

    /// Clone-mutate-publish the snapshot under write_mu_ (RCU write side).
    template <class Fn>
    void mutate_snapshot(Fn&& fn);

    /// Drops invalidated-file locations from a snapshot's trigram maps and
    /// clears the invalidation set. Operates on the writer's private clone.
    static void cleanup_snapshot(Snapshot& snap);

    /// Filters candidate results by match count and invalidation status,
    /// reading the caller's loaded snapshot.
    std::vector<FileID> filter_and_return_candidates(
        const Snapshot& snap,
        const absl::flat_hash_map<FileID, int>& file_trigram_counts,
        int total_trigrams) const;
};

// -- Free functions for trigram extraction ------------------------------------

/// Returns true if all bytes are ASCII (< 128).
bool is_pure_ascii(std::string_view content);

/// Returns true for content the trigram index handles badly: minified /
/// generated single-line bundles and large high-entropy data files. Such
/// files cost per-occurrence storage far out of proportion to their size
/// and their near-random trigram sets defeat prefiltering. Callers index
/// them via mark_unfiltered instead.
bool is_trigram_hostile(std::string_view content);

/// True when the file carries a LARGE machine-generated payload section:
/// compressed/base64 data (high byte entropy, ~6 bits/byte vs ~4.5 for
/// code) or hash/hex dumps (NOT high-entropy -- hex is a 16-symbol
/// alphabet at ~4 bits/byte -- but token-dense, digit-heavy, and
/// whitespace-starved in a way prose and code never are). Classified per
/// 4 KB block; triggers once qualifying blocks total >= 16 KB, so a lone
/// embedded key or digest never trips it. Callers use it to apply the
/// data-file postings token cap to files whose EXTENSION says code but
/// whose content says payload (generated .ts blobs, vendored wordlists,
/// inlined assets).
bool has_high_entropy_section(std::string_view content);

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
