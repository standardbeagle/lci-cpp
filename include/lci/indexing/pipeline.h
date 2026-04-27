#pragma once

#include <atomic>
#include <memory>

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
    void run();

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

  private:
    Config config_;
    std::shared_ptr<FileService> file_service_;
    TrigramIndex* trigram_index_;
    ReferenceTracker* ref_tracker_;
    PostingsIndex* postings_index_;

    ProgressTracker progress_;
    FileIntegrator integrator_;
    std::atomic<bool> stop_flag_{false};
};

}  // namespace lci
