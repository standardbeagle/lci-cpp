#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <lci/config.h>
#include <lci/core/file_service.h>
#include <lci/core/trigram.h>
#include <lci/indexing/binary_detector.h>
#include <lci/indexing/pipeline_types.h>

namespace lci {

/// Thread-safe bounded queue with back-pressure support.
///
/// Producers block when the queue reaches capacity, preventing OOM
/// on large codebases. Consumers block when the queue is empty.
template <typename T>
class BoundedQueue {
  public:
    explicit BoundedQueue(int capacity)
        : capacity_(capacity) {}

    /// Pushes an item, blocking if the queue is full.
    /// Returns false if the queue has been closed.
    bool push(T item) {
        std::unique_lock lock(mu_);
        not_full_.wait(lock, [this] { return queue_.size() < static_cast<size_t>(capacity_) || closed_; });
        if (closed_) return false;
        queue_.push(std::move(item));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    /// Pops an item, blocking if the queue is empty.
    /// Returns false if the queue is closed and empty.
    bool pop(T& out) {
        std::unique_lock lock(mu_);
        not_empty_.wait(lock, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    /// Closes the queue, unblocking all waiting threads.
    void close() {
        {
            std::lock_guard lock(mu_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    /// Returns the current number of items in the queue.
    int size() const {
        std::lock_guard lock(mu_);
        return static_cast<int>(queue_.size());
    }

  private:
    mutable std::mutex mu_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::queue<T> queue_;
    int capacity_;
    bool closed_{};
};

/// Processes files in parallel using a thread pool.
///
/// Workers read FileTask items from an input queue, parse and extract
/// symbols, bucket trigrams, and write ProcessedFile results to an
/// output queue. Back-pressure is enforced through bounded queues.
class FileProcessor {
  public:
    FileProcessor(const Config& config,
                  std::shared_ptr<FileService> file_service,
                  TrigramIndex* trigram_index);

    /// Processes all tasks from the input queue, writing results to the output queue.
    /// Spawns worker_count threads (0 = auto-detect from hardware concurrency).
    /// Blocks until all tasks are processed and workers have joined.
    void process(BoundedQueue<FileTask>& tasks,
                 BoundedQueue<ProcessedFile>& results,
                 int worker_count = 0);

  private:
    const Config& config_;
    std::shared_ptr<FileService> file_service_;
    BinaryDetector binary_detector_;
    TrigramIndex* trigram_index_;

    void worker_loop(int worker_id,
                     BoundedQueue<FileTask>& tasks,
                     BoundedQueue<ProcessedFile>& results);

    ProcessedFile process_file(int worker_id, const FileTask& task);
};

}  // namespace lci
