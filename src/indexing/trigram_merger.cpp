#include <lci/indexing/trigram_merger.h>

namespace lci {

TrigramMergerPipeline::TrigramMergerPipeline(
    TrigramIndex& trigram_index, int merger_count)
    : trigram_index_(trigram_index),
      storage_(static_cast<uint16_t>(trigram_index.get_bucket_count())),
      merger_count_(merger_count <= 0 ? 16 : merger_count),
      buckets_per_merger_(0),
      input_queue_(merger_count_ * 32) {
    int bucket_count = trigram_index_.get_bucket_count();
    buckets_per_merger_ = bucket_count / merger_count_;
    if (buckets_per_merger_ == 0) {
        buckets_per_merger_ = 1;
    }
}

TrigramMergerPipeline::~TrigramMergerPipeline() {
    shutdown();
}

void TrigramMergerPipeline::start() {
    workers_.reserve(merger_count_);
    for (int i = 0; i < merger_count_; ++i) {
        int bucket_start = i * buckets_per_merger_;
        int bucket_end = bucket_start + buckets_per_merger_;
        if (bucket_end > trigram_index_.get_bucket_count()) {
            bucket_end = trigram_index_.get_bucket_count();
        }
        workers_.emplace_back(&TrigramMergerPipeline::worker_loop, this,
                              i, bucket_start, bucket_end);
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

void TrigramMergerPipeline::worker_loop(int /*worker_id*/,
                                         int bucket_start, int bucket_end) {
    BucketedTrigramResult result;
    while (input_queue_.pop(result)) {
        storage_.merge_bucket_data_for_worker(
            result, bucket_start, bucket_end,
            &trigram_index_.get_allocator());
    }
}

}  // namespace lci
