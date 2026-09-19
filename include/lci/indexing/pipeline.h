#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <utility>
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

/// Bounded reorder buffer for pipeline results keyed by file_id.
///
/// Workers finish files out of order; integration must run in ascending
/// file_id order so symbol_id assignment is deterministic. The old drain
/// loop buffered the WHOLE corpus, then sorted it: peak transient memory
/// was every ProcessedFile (symbols+refs+tokens+bloom) alive at once, and
/// the integrator idled for the entire parse phase. This buffer releases
/// the contiguous ascending prefix to its sink the moment the next
/// expected id arrives, so the held set is bounded by the disorder span
/// of the worker pool instead of the corpus.
///
/// Gap policy: a file_id that never produces a result (batch-load failure —
/// the producer skipped the task) is signalled with note_missing() when the
/// error result does arrive; ids absent without any result (load failures
/// carry no ProcessedFile at all) stall the prefix until close(), which
/// flushes everything still held in ascending order. Global release order
/// is therefore ALWAYS ascending by file_id — the flush tail is strictly
/// above the released prefix — which is exactly the order the old
/// whole-corpus sort produced.
class ReorderFileBuffer {
  public:
    explicit ReorderFileBuffer(std::function<void(ProcessedFile&&)> sink)
        : sink_(std::move(sink)) {}

    /// Takes ownership of one result; releases it (and any now-contiguous
    /// successors) to the sink as soon as file_id order allows.
    void push(ProcessedFile&& pf) {
        pending_.emplace(pf.file_id, std::move(pf));
        release_prefix();
    }

    /// Marks a result that will never enter the buffer (error/skipped file)
    /// so the prefix advances past it immediately.
    void note_missing(FileID id) {
        if (id == next_) {
            ++next_;
            release_prefix();
        }
    }

    /// Flushes everything still held, ascending. Called once the result
    /// queue is closed and drained — after this the sink has seen every
    /// file in full ascending file_id order.
    void close() {
        for (auto& [id, pf] : pending_) sink_(std::move(pf));
        pending_.clear();
    }

    size_t held() const { return pending_.size(); }

  private:
    void release_prefix() {
        for (;;) {
            auto it = pending_.find(next_);
            if (it == pending_.end()) return;
            sink_(std::move(it->second));
            pending_.erase(it);
            ++next_;
        }
    }

    std::function<void(ProcessedFile&&)> sink_;
    std::map<FileID, ProcessedFile> pending_;
    // The producer assigns FileIDs densely from 1 (id 0 is the "load
    // failed, never dispatched" sentinel the pipeline skips), so every
    // dispatched id is in [1, K] and yields exactly one result. Starting the
    // ascending release from 1 — not from the first arrival — is what keeps
    // the prefix contiguous: anchoring to the first arrival would strand
    // every smaller id until close().
    FileID next_ = 1;
};

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

    /// Stage 1+2 only: scan the corpus and parse every file, releasing each
    /// ProcessedFile to an internal in-file_id-order buffer as soon as the
    /// worker pool's completion order allows (see ReorderFileBuffer). Writes
    /// NOTHING into the trigram/reference/postings indexes — that is
    /// integrate()'s job. Splitting the two lets a caller keep the
    /// previously published generation intact for the whole (long) parse
    /// phase and clear the sub-indexes only once the run is known to commit.
    void scan_and_parse();

    /// Stage 3: drain the in-order buffer built by scan_and_parse() into the
    /// indexes, releasing each file's payload as it is merged so the
    /// integrator phase's transient memory decays with progress.
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
