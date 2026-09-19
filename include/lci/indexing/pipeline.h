#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <lci/config.h>
#include <lci/core/file_service.h>
#include <lci/core/reference_tracker.h>
#include <lci/core/trigram.h>
#include <lci/indexing/pipeline_integrator.h>
#include <lci/indexing/pipeline_processor.h>
#include <lci/indexing/pipeline_progress.h>
#include <lci/indexing/pipeline_scanner.h>
#include <lci/indexing/pipeline_types.h>

namespace lci {

class SideEffectAnalyzer;

/// Orchestrates the 3-stage indexing pipeline: Scanner -> Processor -> Integrator.
///
/// Each stage communicates through bounded queues that provide back-pressure.
/// The pipeline supports cancellation via request_stop() and reports progress
/// through a ProgressTracker.
class Pipeline {
  public:
    Pipeline(const Config& config,
             std::shared_ptr<FileService> file_service,
             TrigramIndex* trigram_index,
             ReferenceTracker* ref_tracker,
             PostingsIndex* postings_index);

    /// Runs the full pipeline: scan, process, integrate.
    /// Blocks until all stages complete or the pipeline is stopped.
    /// Equivalent to scan_and_parse() followed by integrate() when the scan
    /// succeeded and no stop was requested.
    void run();

    /// Stage 1+2 only: scan the corpus and parse every file into an internal
    /// buffer. Writes NOTHING into the trigram/reference/postings indexes —
    /// that is integrate()'s job. Splitting the two lets a caller keep the
    /// previously published generation intact for the whole (long) parse
    /// phase and clear the sub-indexes only once the run is known to commit.
    void scan_and_parse();

    /// Stage 3: drain the buffer built by scan_and_parse() into the indexes.
    /// Idempotent-by-consumption: the buffer is emptied, so a second call
    /// integrates nothing.
    void integrate();

    /// Records side effects during extraction into `target` (see
    /// FileProcessor::set_side_effect_target). Set before run().
    void set_side_effect_target(SideEffectAnalyzer* target) {
        side_effect_target_ = target;
    }

    /// Requests graceful cancellation of the pipeline.
    void request_stop();

    /// Returns true if stop has been requested.
    bool stop_requested() const;

    /// Returns a snapshot of current progress.
    IndexingProgress get_progress() const;

    /// Returns the integrator for post-pipeline queries (file mapping, etc.).
    FileIntegrator& integrator();
    const FileIntegrator& integrator() const;

    /// Returns the progress tracker for external monitoring.
    ProgressTracker& progress_tracker();
    const ProgressTracker& progress_tracker() const;

    /// Non-empty when run() aborted because the scan rejected the corpus
    /// (index.overflow_policy "reject"). Callers fail the index run on it.
    const std::string& scan_error() const { return scan_error_; }

    /// Per-file failures reported by FileService::batch_load_from_disk during
    /// the producer's batch loads — files that could not be opened or exceeded
    /// the size limit. Non-empty means the run is INCOMPLETE: it integrated the
    /// surviving files (a partial-load run DOES publish, unlike a scan-reject),
    /// but the index silently covers less than the whole corpus. Callers must
    /// not treat a successful-looking run with a non-empty list as a complete
    /// index; this is the channel that used to be dropped on the floor.
    const std::vector<Error>& load_failures() const { return load_failures_; }

  private:
    Config config_;
    std::shared_ptr<FileService> file_service_;
    TrigramIndex* trigram_index_;
    ReferenceTracker* ref_tracker_;
    PostingsIndex* postings_index_;

    SideEffectAnalyzer* side_effect_target_{};

    ProgressTracker progress_;
    FileIntegrator integrator_;
    /// Parsed-but-not-yet-integrated results, sorted by file_id by
    /// scan_and_parse() so symbol_id assignment follows scan order.
    std::vector<ProcessedFile> buffered_;
    std::atomic<bool> stop_flag_{false};
    std::string scan_error_;
    std::vector<Error> load_failures_;
};

}  // namespace lci
