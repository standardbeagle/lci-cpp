#include <lci/indexing/pipeline_processor.h>

#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

namespace lci {

FileProcessor::FileProcessor(
    const Config& config,
    std::shared_ptr<FileService> file_service,
    TrigramIndex* trigram_index)
    : config_(config),
      file_service_(std::move(file_service)),
      trigram_index_(trigram_index) {}

void FileProcessor::process(
    BoundedQueue<FileTask>& tasks,
    BoundedQueue<ProcessedFile>& results,
    int worker_count) {

    if (worker_count <= 0) {
        worker_count = static_cast<int>(std::thread::hardware_concurrency());
        if (worker_count < 1) worker_count = 4;
    }

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (int i = 0; i < worker_count; ++i) {
        workers.emplace_back(&FileProcessor::worker_loop, this,
                             i, std::ref(tasks), std::ref(results));
    }

    for (auto& w : workers) w.join();
    results.close();
}

void FileProcessor::worker_loop(
    int worker_id,
    BoundedQueue<FileTask>& tasks,
    BoundedQueue<ProcessedFile>& results) {

    FileTask task;
    while (tasks.pop(task)) {
        auto result = process_file(worker_id, task);
        if (!results.push(std::move(result))) return;
    }
}

ProcessedFile FileProcessor::process_file(int /*worker_id*/,
                                          const FileTask& task) {
    auto start = std::chrono::steady_clock::now();
    ProcessedFile result;
    result.path = task.path;
    result.language = task.language;
    result.stage = "parsing";

    // Load file through FileService
    auto load_result = file_service_->load_file_from_disk(task.path);
    if (!load_result.has_value()) {
        result.has_error = true;
        result.error = load_result.error();
        result.stage = "loading";
        result.duration = std::chrono::steady_clock::now() - start;
        return result;
    }

    FileID file_id = load_result.value();
    if (file_id == 0) {
        result.stage = "directory_skipped";
        result.duration = std::chrono::steady_clock::now() - start;
        return result;
    }

    auto content = file_service_->get_content(file_id);

    // Defense-in-depth: binary check on loaded content
    if (binary_detector_.is_binary_by_magic_number(content)) {
        result.has_error = true;
        result.error.type = ErrorType::Indexing;
        result.error.message = "binary file detected by magic number";
        result.error.file_path = task.path;
        result.stage = "binary_detection";
        result.duration = std::chrono::steady_clock::now() - start;
        return result;
    }

    result.file_id = file_id;

    // Bucket trigrams during processing (zero-lock per-file)
    if (trigram_index_ != nullptr && content.size() >= 3) {
        auto bucketed = trigram_index_->create_bucketed_result(file_id);
        int bucket_count = trigram_index_->get_bucket_count();

        int estimated_per_bucket;
        if (content.size() < 512) {
            estimated_per_bucket = 4;
        } else {
            int est_unique = static_cast<int>(content.size()) / 10;
            if (est_unique < 100) est_unique = 100;
            estimated_per_bucket = est_unique / bucket_count + 2;
            if (estimated_per_bucket < 8) estimated_per_bucket = 8;
        }

        for (int i = 0; i < bucket_count; ++i) {
            bucketed.buckets[i].trigrams.reserve(estimated_per_bucket);
        }

        auto bytes = reinterpret_cast<const uint8_t*>(content.data());
        for (size_t i = 0; i + 2 < content.size(); ++i) {
            uint32_t trigram = (uint32_t(bytes[i]) << 16) |
                               (uint32_t(bytes[i + 1]) << 8) |
                               uint32_t(bytes[i + 2]);
            uint16_t bucket_id = trigram_index_->get_bucket_for_trigram(trigram);
            bucketed.buckets[bucket_id].trigrams[trigram].push_back(
                static_cast<uint32_t>(i));
        }

        result.bucketed_trigrams = std::move(bucketed);
    } else if (trigram_index_ != nullptr) {
        result.bucketed_trigrams = trigram_index_->create_bucketed_result(file_id);
    }

    result.stage = "completed";
    result.duration = std::chrono::steady_clock::now() - start;
    return result;
}

}  // namespace lci
