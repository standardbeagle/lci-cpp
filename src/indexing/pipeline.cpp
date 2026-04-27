#include <lci/indexing/pipeline.h>

#include <thread>

namespace lci {

Pipeline::Pipeline(const Config& config,
                   std::shared_ptr<FileService> file_service,
                   TrigramIndex* trigram_index,
                   ReferenceTracker* ref_tracker,
                   PostingsIndex* postings_index)
    : config_(config),
      file_service_(std::move(file_service)),
      trigram_index_(trigram_index),
      ref_tracker_(ref_tracker),
      postings_index_(postings_index),
      integrator_(trigram_index, ref_tracker, postings_index) {}

void Pipeline::run() {
    // Stage 1: Scan files.
    FileScanner scanner(config_);
    auto tasks = scanner.scan();

    if (stop_flag_.load(std::memory_order_acquire)) return;

    int file_count = static_cast<int>(tasks.size());
    progress_.set_total(file_count);

    if (file_count == 0) return;

    // Calculate queue sizes based on file count.
    auto [task_buf, result_buf] = calculate_optimal_channel_buffers(file_count);
    BoundedQueue<FileTask> task_queue(task_buf);
    BoundedQueue<ProcessedFile> result_queue(result_buf);

    // Producer: feed scanned tasks into the task queue.
    std::thread producer([&] {
        for (auto& task : tasks) {
            if (stop_flag_.load(std::memory_order_acquire)) break;
            progress_.increment_scanned();
            if (!task_queue.push(std::move(task))) break;
        }
        task_queue.close();
    });

    // Stage 2: Process files in parallel (runs in its own thread pool).
    FileProcessor processor(config_, file_service_, trigram_index_);
    std::thread process_thread([&] {
        processor.process(task_queue, result_queue);
    });

    // Stage 3: Integrate results (runs on this thread).
    ProcessedFile result;
    while (result_queue.pop(result)) {
        if (stop_flag_.load(std::memory_order_acquire)) break;
        progress_.increment_processed(result.path);
        if (!result.has_error && result.file_id != 0) {
            integrator_.integrate_file(result);
            progress_.increment_integrated();
        } else if (result.has_error) {
            Error err;
            err.type = ErrorType::Indexing;
            err.file_path = result.path;
            err.message = result.error.message;
            err.operation = result.stage;
            progress_.add_error(std::move(err));
        }
    }

    producer.join();
    process_thread.join();

    // If stop was requested, close queues to unblock any waiting threads.
    if (stop_flag_.load(std::memory_order_acquire)) {
        task_queue.close();
        result_queue.close();
    }
}

void Pipeline::request_stop() {
    stop_flag_.store(true, std::memory_order_release);
}

bool Pipeline::stop_requested() const {
    return stop_flag_.load(std::memory_order_acquire);
}

IndexingProgress Pipeline::get_progress() const {
    return progress_.get_progress();
}

FileIntegrator& Pipeline::integrator() {
    return integrator_;
}

const FileIntegrator& Pipeline::integrator() const {
    return integrator_;
}

ProgressTracker& Pipeline::progress_tracker() {
    return progress_;
}

const ProgressTracker& Pipeline::progress_tracker() const {
    return progress_;
}

}  // namespace lci
