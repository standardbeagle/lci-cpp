#include <lci/core/trigram.h>

#include <cstddef>

namespace lci {

// -- Bucketing utilities for parallel trigram processing ----------------------

namespace {

/// Assigns a trigram hash to a bucket using the low bits of the hash.
/// This produces uniform distribution across power-of-two bucket counts.
uint16_t bucket_for_hash(uint32_t trigram_hash, uint32_t mask) {
    return static_cast<uint16_t>(trigram_hash & mask);
}

}  // namespace

/// Distributes extracted trigrams into buckets for parallel merging.
/// Each bucket receives only the trigrams whose hash maps to that bucket,
/// enabling lock-free parallel processing across bucket ranges.
BucketedTrigramResult bucket_trigrams(
    FileID file_id,
    const absl::flat_hash_map<int, uint32_t>& trigrams,
    int bucket_count) {
    BucketedTrigramResult result;
    result.file_id = file_id;
    result.buckets.resize(static_cast<size_t>(bucket_count));

    uint32_t mask = static_cast<uint32_t>(bucket_count) - 1;

    for (const auto& [offset, trigram_hash] : trigrams) {
        uint16_t bid = bucket_for_hash(trigram_hash, mask);
        auto& bucket_data = result.buckets[bid];
        bucket_data.trigrams[trigram_hash].push_back(
            static_cast<uint32_t>(offset));
    }

    return result;
}

}  // namespace lci
