#include <lci/indexing/pipeline.h>

#include <algorithm>
#include <thread>
#include <utility>
#include <vector>

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

    // Producer: pre-load files into the content store *in scan order* so
    // FileIDs are assigned deterministically (alphabetical within a
    // priority tier). Without this, worker threads race to call
    // load_file_from_disk and the resulting file_id assignment depends
    // on thread scheduling — which then propagates into symbol_id
    // ordering and HTTP/MCP response ordering. Loading here adds a
    // small serialization point but keeps per-file parsing in workers.
    std::thread producer([&] {
        for (auto& task : tasks) {
            if (stop_flag_.load(std::memory_order_acquire)) break;
            progress_.increment_scanned();
            (void)file_service_->load_file_from_disk(task.path);
            if (!task_queue.push(std::move(task))) break;
        }
        task_queue.close();
    });

    // Stage 2: Process files in parallel (runs in its own thread pool).
    FileProcessor processor(config_, file_service_, trigram_index_);
    std::thread process_thread([&] {
        processor.process(task_queue, result_queue);
    });

    // Stage 3: Integrate results (runs on this thread). Buffer all
    // ProcessedFile outputs from the worker pool, then sort by file_id
    // (assigned deterministically by the producer above) so symbol_id
    // assignment in ref_tracker.process_file follows the same scan
    // order. This mirrors Go's reference indexer ordering and keeps
    // HTTP / MCP responses bit-stable across runs.
    std::vector<ProcessedFile> buffered;
    {
        ProcessedFile result;
        while (result_queue.pop(result)) {
            if (stop_flag_.load(std::memory_order_acquire)) break;
            progress_.increment_processed(result.path);
            if (result.has_error) {
                Error err;
                err.type = ErrorType::Indexing;
                err.file_path = result.path;
                err.message = result.error.message;
                err.operation = result.stage;
                progress_.add_error(std::move(err));
                continue;
            }
            if (result.file_id == 0) continue;
            buffered.push_back(std::move(result));
        }
    }

    std::sort(buffered.begin(), buffered.end(),
              [](const ProcessedFile& a, const ProcessedFile& b) {
                  return a.file_id < b.file_id;
              });

    for (auto& result : buffered) {
        integrator_.integrate_file(result);
        progress_.increment_integrated();
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
