#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <absl/container/flat_hash_map.h>

#include <lci/core/file_content_store.h>
#include <lci/core/reference_tracker.h>
#include <lci/core/symbol_store.h>
#include <lci/core/trigram.h>
#include <lci/indexing/pipeline_processor.h>
#include <lci/indexing/pipeline_types.h>
#include <lci/indexing/trigram_merger.h>
#include <lci/types.h>

namespace lci {

/// Merges ProcessedFile results into all sub-indexes.
///
/// The integrator is the Stage 3 consumer in the indexing pipeline.  It
/// reads ProcessedFile values from a BoundedQueue and feeds each file's
/// data into the trigram index, symbol store (via ReferenceTracker),
/// postings index, and symbol location index.
///
/// File updates are handled by removing stale data from every sub-index
/// before re-inserting the new data.
///
/// Thread safety: integrate() must be called from a single thread.
/// Sub-indexes are accessed through their own thread-safety guarantees.
class FileIntegrator {
  public:
    /// Creates an integrator wired to the given sub-indexes.
    /// All pointer parameters are non-owning; callers must ensure they
    /// outlive the integrator.
    FileIntegrator(TrigramIndex* trigram_index,
                   ReferenceTracker* ref_tracker,
                   PostingsIndex* postings_index);

    // -- Optional sub-index setters ------------------------------------------

    void set_file_content_store(FileContentStore* store);
    void set_symbol_location_index(SymbolLocationIndex* index);

    // -- Merger pipeline control ---------------------------------------------

    /// Enables the lock-free trigram merger pipeline with the given worker count.
    void enable_merger_pipeline(int merger_count);

    /// Disables and shuts down the merger pipeline.
    void disable_merger_pipeline();

    /// Returns merger pipeline statistics, or default stats if disabled.
    MergerStats get_merger_stats() const;

    // -- Integration ---------------------------------------------------------

    /// Reads all ProcessedFile entries from the queue and merges them into
    /// the sub-indexes.  Blocks until the queue is closed and drained.
    void integrate(BoundedQueue<ProcessedFile>& results);

    /// Integrates a single ProcessedFile into the sub-indexes.
    void integrate_file(ProcessedFile& file);

    /// Removes all data for a file path from every sub-index.
    void remove_file(const std::string& path);

    // -- File mapping --------------------------------------------------------

    /// Returns the FileID for a path, or 0 if not tracked.
    FileID path_to_id(const std::string& path) const;

    /// Returns the path for a FileID, or empty string if not tracked.
    const std::string& id_to_path(FileID file_id) const;

    /// Returns the number of integrated files.
    int file_count() const;

  private:
    TrigramIndex* trigram_index_;
    ReferenceTracker* ref_tracker_;
    PostingsIndex* postings_index_;
    FileContentStore* file_content_store_{};
    SymbolLocationIndex* symbol_location_index_{};

    std::unique_ptr<TrigramMergerPipeline> merger_pipeline_;
    bool use_merger_pipeline_{};

    absl::flat_hash_map<std::string, FileID> file_map_;
    absl::flat_hash_map<FileID, std::string> reverse_file_map_;

    std::atomic<uint32_t> file_id_counter_{0};
    int integrated_count_{};

    static const std::string kEmptyString;

    void merge_trigrams(ProcessedFile& file);
    void merge_symbols(ProcessedFile& file);
    void merge_postings(ProcessedFile& file);
    void remove_stale_data(FileID file_id);
};

}  // namespace lci
