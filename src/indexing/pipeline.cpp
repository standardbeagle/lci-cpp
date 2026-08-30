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
    auto scan_result = scanner.scan();
    if (!scan_result.error.empty()) {
        scan_error_ = scan_result.error;
        return;
    }
    if (scan_result.skipped_files > 0) {
        // Reduced overflow policy: partial index, loudly. Silent truncation
        // would read as "covered everything".
        std::fprintf(stderr,
                     "lci: corpus budget reached — indexing %zu files, "
                     "skipping %d lower-priority files (%.1f MB); raise "
                     "index.max_total_size_mb / index.max_file_count or "
                     "tighten excludes for full coverage\n",
                     scan_result.tasks.size(), scan_result.skipped_files,
                     static_cast<double>(scan_result.skipped_bytes) /
                         (1024.0 * 1024.0));
    }
    auto tasks = std::move(scan_result.tasks);

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
    // ordering and HTTP/MCP response ordering.
    //
    // Batched loading: per-file add_file rewrites the FileContentSnapshot
    // end-to-end (RCU copy-on-write). Calling it N times in a row is
    // O(N²) in snapshot size — perf showed 13% CPU in malloc+free and
    // 6.5% in FileContentSnapshot shared_ptr ref counting, both rooted
    // in this per-file rewrite. Group into chunks so each chunk pays
    // one snapshot rewrite, then push the chunk's tasks to workers.
    constexpr size_t kLoadBatchSize = 256;
    std::thread producer([&] {
        std::vector<std::string> batch_paths;
        std::vector<FileTask> batch_tasks;
        batch_paths.reserve(kLoadBatchSize);
        batch_tasks.reserve(kLoadBatchSize);

        auto flush = [&]() {
            if (batch_paths.empty()) return true;
            auto ids = file_service_->batch_load_from_disk(batch_paths);
            for (size_t i = 0; i < batch_tasks.size(); ++i) {
                if (stop_flag_.load(std::memory_order_acquire)) return false;
                progress_.increment_scanned();
                // Carry the producer-assigned FileID into the task so
                // the worker can skip the redundant load_file_from_disk
                // snapshot copy on the inner loop.
                if (i < ids.size()) batch_tasks[i].preloaded_id = ids[i];
                if (!task_queue.push(std::move(batch_tasks[i]))) return false;
            }
            batch_paths.clear();
            batch_tasks.clear();
            return true;
        };

        for (auto& task : tasks) {
            if (stop_flag_.load(std::memory_order_acquire)) break;
            batch_paths.push_back(task.path);
            batch_tasks.push_back(std::move(task));
            if (batch_paths.size() >= kLoadBatchSize) {
                if (!flush()) break;
            }
        }
        flush();
        task_queue.close();
    });

    // Stage 2: Process files in parallel (runs in its own thread pool).
    // Worker count is read from config.performance — parallel_file_workers
    // first (more specific), falling back to max_goroutines, both honoring
    // the 0 = auto-detect contract. Wiring this prevents N-test ctest
    // runs from each defaulting to hw_concurrency() and oversubscribing
    // the CPU by N×.
    FileProcessor processor(config_, file_service_, trigram_index_);
    processor.set_side_effect_target(side_effect_target_);
    int worker_count = config_.performance.parallel_file_workers;
    if (worker_count <= 0) worker_count = config_.performance.max_goroutines;
    std::thread process_thread([&, worker_count] {
        processor.process(task_queue, result_queue, worker_count);
    });

    // The async trigram merger is no longer enabled: workers stopped
    // producing bucketed trigrams (dead ShardedTrigramStorage feed — see
    // pipeline_processor.cpp), so there is nothing to merge.

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

            // Surface a symbol-extraction skip as a recoverable warning on
            // the same channel as hard errors. UnsupportedGrammar is left
            // out deliberately: it is the expected outcome for every non-
            // source file in the corpus, and reporting it would bury the
            // four reasons that indicate something actually went wrong.
            if (result.parse_skip_reason != ParseSkipReason::None &&
                result.parse_skip_reason !=
                    ParseSkipReason::UnsupportedGrammar) {
                Error warn;
                warn.type = ErrorType::Parse;
                warn.file_path = result.path;
                warn.operation = "symbol_extraction";
                warn.recoverable = true;
                warn.message =
                    "indexed as text only, no symbols extracted: " +
                    std::string(to_string(result.parse_skip_reason));
                progress_.add_error(std::move(warn));
            }

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

    // On stop the integrator loop above breaks without draining, so
    // workers can be blocked pushing into the bounded result_queue (and
    // the producer into task_queue). Close both queues BEFORE joining —
    // a blocked push then returns false and the threads exit; closing
    // after the joins (the previous order) deadlocked run() forever.
    if (stop_flag_.load(std::memory_order_acquire)) {
        task_queue.close();
        result_queue.close();
    }

    producer.join();
    process_thread.join();
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
