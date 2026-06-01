#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
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

/// Parallel trigram merger pipeline with per-bucket locking.
///
/// Submitted BucketedTrigramResults flow through a single back-pressured
/// BoundedQueue (bounds memory on large codebases). Each worker pops a whole
/// result and merges ALL of its non-empty buckets, taking that bucket's
/// fine-grained lock for the write. Distinct buckets merge in parallel; two
/// workers touching the same bucket serialize only on that one lock.
///
/// (Prior design partitioned buckets across workers and relied on a single
/// pop per result — but a shared consume-once queue hands each result to just
/// ONE worker, so every bucket outside that worker's slice was silently
/// dropped: results were merged only partially and nondeterministically. The
/// per-bucket lock replaces that broken "bucket isolation" with correct,
/// still-parallel merging.)
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
    int bucket_count_;

    BoundedQueue<BucketedTrigramResult> input_queue_;
    std::vector<std::thread> workers_;
    // One lock per bucket. Confined to the merge write path; the search read
    // path never takes these (index-vs-search concurrency is the RCU epic's
    // concern, not the merger's). unique_ptr keeps the pipeline movable-free
    // without making std::mutex a copy/move hazard.
    std::unique_ptr<std::mutex[]> bucket_locks_;

    std::atomic<bool> shutdown_flag_{false};
    std::atomic<int64_t> failed_files_{0};

    void worker_loop(int worker_id);
};

}  // namespace lci
