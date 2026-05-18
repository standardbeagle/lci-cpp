#include <lci/indexing/pipeline_processor.h>

#include <lci/core/reference_tracker.h>
#include <lci/parser/parser.h>
#include <lci/parser/parser_pool.h>
#include <lci/parser/unified_extractor.h>

#include <tree_sitter/api.h>

#include <chrono>
#include <filesystem>
#include <thread>
#include <utility>
#include <vector>

namespace lci {

namespace {

/// Extracts symbols, references, and scopes from a file's content via
/// tree-sitter + UnifiedExtractor. Populates `result` in place. Falls
/// through silently if the language is unsupported or parsing fails so
/// that trigram/postings indexing still proceeds.
void run_unified_extraction(ProcessedFile& result,
                            std::string_view content,
                            const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    if (ext.empty()) return;

    parser::Language lang{};
    if (!parser::language_from_extension(ext, lang)) {
        return;  // Unsupported language: trigrams still index for text search.
    }

    parser::PooledParser parser_guard(lang);
    if (!parser_guard) return;

    TSTree* tree = ts_parser_parse_string(
        parser_guard.get(), nullptr, content.data(),
        static_cast<uint32_t>(content.size()));
    if (tree == nullptr) return;

    parser::UnifiedExtractor extractor;
    extractor.init(content, result.file_id, ext, path);
    extractor.extract(tree);
    auto extracted = extractor.get_results();
    ts_tree_delete(tree);

    // Build a position-keyed metadata index so the integrator can enrich
    // EnhancedSymbol records (complexity, signature, doc comment) without
    // changing the ReferenceTracker API. Symbol coordinates and the
    // declaration / complexity keys all use 1-based lines and columns.
    result.symbol_metadata.reserve(extracted.symbols.size());
    for (const auto& sym : extracted.symbols) {
        ProcessedSymbolMetadata meta;
        meta.line = sym.line;
        meta.column = sym.column;
        for (const auto& [pk, cx] : extracted.complexity) {
            if (pk.line == sym.line && pk.column == sym.column) {
                meta.complexity = cx;
                break;
            }
        }
        auto [signature, doc_comment] =
            extractor.lookup_declaration(sym.line, sym.column);
        meta.signature.assign(signature);
        meta.doc_comment.assign(doc_comment);
        result.symbol_metadata.push_back(std::move(meta));
    }

    result.symbols = std::move(extracted.symbols);
    result.references = std::move(extracted.references);
    result.scopes = std::move(extracted.scopes);
}

}  // namespace

FileProcessor::FileProcessor(
    const Config& config,
    std::shared_ptr<FileService> file_service,
    TrigramIndex* trigram_index)
    : config_(config),
      file_service_(std::move(file_service)),
      trigram_index_(trigram_index) {
    // Warm tree-sitter grammar tables on the main thread so the first
    // parse on a worker thread does not pay a cold-start cost. The
    // grammar init functions are documented thread-safe; touching them
    // and driving a one-byte parse keeps the worker pool's first batch
    // hot without measurable overhead on small corpora.
    for (int i = 0; i < parser::kLanguageCount; ++i) {
        auto lang = static_cast<parser::Language>(i);
        const TSLanguage* ts_lang = parser::get_ts_language(lang);
        if (ts_lang == nullptr) continue;

        parser::UniqueParser p = parser::make_parser(lang);
        if (!p) continue;

        constexpr const char* kWarmInput = " ";
        TSTree* tree = ts_parser_parse_string(p.get(), nullptr, kWarmInput, 1);
        if (tree != nullptr) ts_tree_delete(tree);
    }
}

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

    // Producer-assigned FileID skips the redundant store_->load_file
    // snapshot copy. Fallback path covers single-file callers that
    // bypass the producer pipeline (tests, ad-hoc).
    FileID file_id = task.preloaded_id;
    if (file_id == 0) {
        auto load_result = file_service_->load_file_from_disk(task.path);
        if (!load_result.has_value()) {
            result.has_error = true;
            result.error = load_result.error();
            result.stage = "loading";
            result.duration = std::chrono::steady_clock::now() - start;
            return result;
        }
        file_id = load_result.value();
        if (file_id == 0) {
            result.stage = "directory_skipped";
            result.duration = std::chrono::steady_clock::now() - start;
            return result;
        }
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

    // Parse the file and extract symbols, references, and scopes via
    // tree-sitter. This populates the symbol-aware data the integrator
    // feeds into ReferenceTracker. Without this step, browse-file,
    // list-symbols, references, and tree endpoints all return empty.
    run_unified_extraction(result, content, task.path);

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

    // Tokenize for PostingsIndex inline so the per-byte scan + dedup
    // runs in parallel here instead of serially on the integrator
    // thread. FileIntegrator::merge_postings consumes the result via
    // index_file_pretokenized — pure merge, no re-walk of content.
    {
        auto pi_tokens = lci::PostingsIndex::tokenize_content(content);
        result.postings_tokens.reserve(pi_tokens.size());
        for (auto& pt : pi_tokens) {
            ProcessedToken t;
            t.token = std::move(pt.token);
            t.offset = pt.offset;
            result.postings_tokens.push_back(std::move(t));
        }
    }

    result.stage = "completed";
    result.duration = std::chrono::steady_clock::now() - start;
    return result;
}

}  // namespace lci
