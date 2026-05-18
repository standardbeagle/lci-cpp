#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <lci/core/trigram.h>
#include <lci/indexing/pipeline_processor.h>

namespace lci {

/// Statistics about the trigram merger pipeline.
struct MergerStats {
    int buffer_size{};
    int buffer_capacity{};
    int merger_count{};
    double buffer_usage{};
};

/// Lock-free trigram merger pipeline using bucket isolation.
///
/// Each worker thread owns a non-overlapping range of buckets from the
/// ShardedTrigramStorage.  Submitted BucketedTrigramResults are broadcast
/// to all workers through a shared BoundedQueue; each worker merges only
/// the buckets in its assigned range.  Because bucket ranges never overlap,
/// no locks are needed during merging.
///
/// Lifecycle: construct -> start() -> submit() ... -> shutdown().
/// shutdown() is idempotent and safe to call multiple times.
class TrigramMergerPipeline {
  public:
    /// Creates a merger pipeline with the given number of worker threads.
    /// mergerCount <= 0 defaults to 16.
    explicit TrigramMergerPipeline(TrigramIndex& trigram_index,
                                   int merger_count = 16);

    ~TrigramMergerPipeline();

    TrigramMergerPipeline(const TrigramMergerPipeline&) = delete;
    TrigramMergerPipeline& operator=(const TrigramMergerPipeline&) = delete;

    /// Launches worker threads.  Must be called before submit().
    void start();

    /// Submits a bucketed trigram result for merging.
    /// Takes ownership: the BucketedTrigramResult's hash-of-buckets is
    /// moved into the input queue, not copied. Pass via std::move from
    /// a moved-from-okay slot in the caller.
    /// Returns false if the pipeline is shut down or both queues are full.
    bool submit(BucketedTrigramResult&& result);

    /// Gracefully shuts down all workers and drains remaining work.
    /// Idempotent: safe to call multiple times.
    void shutdown();

    /// Returns current pipeline statistics.
    MergerStats get_stats() const;

    /// Returns the number of files that failed to merge.
    int64_t get_failed_file_count() const;

    /// Returns true if any files failed to merge.
    bool has_failures() const;

    /// Returns the underlying sharded storage for query use.
    ShardedTrigramStorage& storage();

  private:
    TrigramIndex& trigram_index_;
    ShardedTrigramStorage storage_;
    int merger_count_;
    int buckets_per_merger_;

    BoundedQueue<BucketedTrigramResult> input_queue_;
    std::vector<std::thread> workers_;

    std::atomic<bool> shutdown_flag_{false};
    std::atomic<int64_t> failed_files_{0};

    void worker_loop(int worker_id, int bucket_start, int bucket_end);
};

}  // namespace lci
