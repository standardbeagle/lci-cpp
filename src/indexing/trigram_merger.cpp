#include <lci/indexing/trigram_merger.h>

namespace lci {

TrigramMergerPipeline::TrigramMergerPipeline(
    TrigramIndex& trigram_index, int merger_count)
    : trigram_index_(trigram_index),
      storage_(static_cast<uint16_t>(trigram_index.get_bucket_count())),
      merger_count_(merger_count <= 0 ? 16 : merger_count),
      bucket_count_(trigram_index.get_bucket_count()),
      input_queue_(merger_count_ * 32),
      bucket_locks_(std::make_unique<std::mutex[]>(
          static_cast<size_t>(bucket_count_ > 0 ? bucket_count_ : 1))) {}

TrigramMergerPipeline::~TrigramMergerPipeline() {
    shutdown();
}

void TrigramMergerPipeline::start() {
    workers_.reserve(merger_count_);
    for (int i = 0; i < merger_count_; ++i) {
        workers_.emplace_back(&TrigramMergerPipeline::worker_loop, this, i);
    }
}

bool TrigramMergerPipeline::submit(BucketedTrigramResult&& result) {
    if (shutdown_flag_.load(std::memory_order_acquire)) {
        return false;
    }
    // Move into the input queue. Caller has yielded ownership.
    if (!input_queue_.push(std::move(result))) {
        failed_files_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void TrigramMergerPipeline::shutdown() {
    bool expected = false;
    if (!shutdown_flag_.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
        return;  // Already shut down.
    }
    input_queue_.close();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

MergerStats TrigramMergerPipeline::get_stats() const {
    int buf_size = input_queue_.size();
    int buf_cap = merger_count_ * 32;
    double usage = buf_cap > 0
        ? static_cast<double>(buf_size) / static_cast<double>(buf_cap) * 100.0
        : 0.0;
    return MergerStats{buf_size, buf_cap, merger_count_, usage};
}

int64_t TrigramMergerPipeline::get_failed_file_count() const {
    return failed_files_.load(std::memory_order_relaxed);
}

bool TrigramMergerPipeline::has_failures() const {
    return failed_files_.load(std::memory_order_relaxed) > 0;
}

ShardedTrigramStorage& TrigramMergerPipeline::storage() {
    return storage_;
}

void TrigramMergerPipeline::worker_loop(int /*worker_id*/) {
    BucketedTrigramResult result;
    while (input_queue_.pop(result)) {
        // Merge every non-empty bucket of this result, each under its own
        // lock so concurrent workers serialize only on shared buckets.
        int n = static_cast<int>(result.buckets.size());
        if (n > bucket_count_) n = bucket_count_;
        for (int bid = 0; bid < n; ++bid) {
            if (result.buckets[static_cast<size_t>(bid)].trigrams.empty()) {
                continue;
            }
            std::lock_guard<std::mutex> lk(bucket_locks_[static_cast<size_t>(bid)]);
            storage_.merge_bucket_data_for_worker(result, bid, bid + 1);
        }
    }
}

}  // namespace lci
