#include <lci/indexing/pipeline_integrator.h>

namespace lci {

const std::string FileIntegrator::kEmptyString;

FileIntegrator::FileIntegrator(TrigramIndex* trigram_index,
                               ReferenceTracker* ref_tracker,
                               PostingsIndex* postings_index)
    : trigram_index_(trigram_index),
      ref_tracker_(ref_tracker),
      postings_index_(postings_index) {}

void FileIntegrator::set_file_content_store(FileContentStore* store) {
    file_content_store_ = store;
}

void FileIntegrator::set_symbol_location_index(SymbolLocationIndex* index) {
    symbol_location_index_ = index;
}

void FileIntegrator::enable_merger_pipeline(int merger_count) {
    if (trigram_index_ == nullptr) return;
    merger_pipeline_ = std::make_unique<TrigramMergerPipeline>(
        *trigram_index_, merger_count);
    merger_pipeline_->start();
    use_merger_pipeline_ = true;
}

void FileIntegrator::disable_merger_pipeline() {
    if (merger_pipeline_) {
        merger_pipeline_->shutdown();
        merger_pipeline_.reset();
    }
    use_merger_pipeline_ = false;
}

MergerStats FileIntegrator::get_merger_stats() const {
    if (merger_pipeline_) {
        return merger_pipeline_->get_stats();
    }
    return MergerStats{};
}

void FileIntegrator::integrate(BoundedQueue<ProcessedFile>& results) {
    ProcessedFile file;
    while (results.pop(file)) {
        if (file.has_error || file.file_id == 0) continue;
        integrate_file(file);
    }
    disable_merger_pipeline();
}

void FileIntegrator::integrate_file(ProcessedFile& file) {
    FileID file_id = file.file_id;

    // Check if this is an update - remove stale data first.
    auto it = file_map_.find(file.path);
    if (it != file_map_.end() && it->second != file_id) {
        remove_stale_data(it->second);
    }

    // Record the file mapping.
    file_map_[file.path] = file_id;
    reverse_file_map_[file_id] = file.path;

    merge_trigrams(file);
    merge_symbols(file);
    merge_postings(file);
    ++integrated_count_;
}

void FileIntegrator::remove_file(const std::string& path) {
    auto it = file_map_.find(path);
    if (it == file_map_.end()) return;
    FileID file_id = it->second;
    remove_stale_data(file_id);
    reverse_file_map_.erase(file_id);
    file_map_.erase(it);
}

FileID FileIntegrator::path_to_id(const std::string& path) const {
    auto it = file_map_.find(path);
    return it != file_map_.end() ? it->second : FileID{0};
}

const std::string& FileIntegrator::id_to_path(FileID file_id) const {
    auto it = reverse_file_map_.find(file_id);
    return it != reverse_file_map_.end() ? it->second : kEmptyString;
}

int FileIntegrator::file_count() const {
    return static_cast<int>(file_map_.size());
}

void FileIntegrator::merge_trigrams(ProcessedFile& file) {
    if (trigram_index_ == nullptr) return;
    if (file.bucketed_trigrams.buckets.empty()) return;

    if (use_merger_pipeline_ && merger_pipeline_) {
        // Move the per-file bucketed map into the merger queue instead
        // of copying it. file.bucketed_trigrams is dead after merge_*
        // returns (subsequent merges don't read it). perf record showed
        // submit's const-ref + internal copy ctor accounting for ~1%
        // of indexing wall on top of the broader heap churn.
        merger_pipeline_->submit(std::move(file.bucketed_trigrams));
    } else {
        trigram_index_->index_file_with_bucketed_trigrams(
            file.bucketed_trigrams);
    }
}

void FileIntegrator::merge_symbols(ProcessedFile& file) {
    if (file.symbols.empty()) return;

    if (ref_tracker_ != nullptr) {
        // Process imports first, then symbols + references + scopes.
        if (file_content_store_ != nullptr) {
            auto content = file_content_store_->get_content(file.file_id);
            ref_tracker_->process_file_imports(
                file.file_id, file.path, content);
        }

        auto enhanced = ref_tracker_->process_file(
            file.file_id, file.path, file.symbols,
            file.references, file.scopes);

        // Enrich EnhancedSymbols with parser-only metadata (complexity,
        // signature, doc comment). ReferenceTracker stores these as zero
        // defaults; we look them up by (line, column) coordinates and
        // write back via the symbol_store update path.
        if (!enhanced.empty() && !file.symbol_metadata.empty()) {
            auto& store = ref_tracker_->symbol_store_mut();
            for (auto& es : enhanced) {
                for (const auto& meta : file.symbol_metadata) {
                    if (meta.line != es.symbol.line ||
                        meta.column != es.symbol.column) {
                        continue;
                    }
                    if (meta.complexity > 0) es.complexity = meta.complexity;
                    if (!meta.signature.empty()) es.signature = meta.signature;
                    if (!meta.doc_comment.empty()) {
                        es.doc_comment = meta.doc_comment;
                    }
                    store.set(es.id, es);
                    break;
                }
            }
        }

        if (symbol_location_index_ != nullptr && !enhanced.empty()) {
            symbol_location_index_->index_file_symbols(
                file.file_id, file.symbols, enhanced);
        }
    } else if (symbol_location_index_ != nullptr) {
        symbol_location_index_->index_file_symbols(
            file.file_id, file.symbols, {});
    }
}

void FileIntegrator::merge_postings(ProcessedFile& file) {
    if (postings_index_ == nullptr) return;

    // Workers tokenize during process_file; the integrator just merges
    // (token, offset) pairs into the shared maps. Per-byte scan +
    // dedup hash map are off the integrator's serial hot path.
    if (file.postings_tokens.empty()) {
        // Fallback for paths that haven't been migrated to the parallel
        // tokenization yet (e.g. legacy index_file callers).
        if (file_content_store_ == nullptr) return;
        auto content = file_content_store_->get_content(file.file_id);
        if (content.empty()) return;
        postings_index_->index_file(file.file_id, content);
        return;
    }

    // Move the worker-built tokens into the postings index, avoiding
    // a per-token string copy. file.postings_tokens is dead after this
    // call (no other integrator stage reads it).
    std::vector<PostingsToken> tokens;
    tokens.reserve(file.postings_tokens.size());
    for (auto& t : file.postings_tokens) {
        tokens.push_back(PostingsToken{std::move(t.token), t.offset});
    }
    postings_index_->index_file_pretokenized(file.file_id, std::move(tokens));
}

void FileIntegrator::remove_stale_data(FileID file_id) {
    if (trigram_index_ != nullptr) {
        trigram_index_->remove_file(file_id);
    }
    if (ref_tracker_ != nullptr) {
        ref_tracker_->remove_file(file_id);
    }
    if (postings_index_ != nullptr) {
        postings_index_->remove_file(file_id);
    }
    if (symbol_location_index_ != nullptr) {
        symbol_location_index_->remove_file(file_id);
    }
}

}  // namespace lci
