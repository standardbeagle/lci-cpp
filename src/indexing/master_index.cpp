#include <lci/indexing/master_index.h>

#include <chrono>
#include <filesystem>

namespace lci {

// -- Construction / Destruction -----------------------------------------------

MasterIndex::MasterIndex(const Config& config)
    : config_(config),
      ref_tracker_(&symbol_location_index_),
      file_content_store_(std::make_shared<FileContentStore>(
          static_cast<int64_t>(config.performance.max_memory_mb) * 1024 * 1024)),
      file_service_(std::make_shared<FileService>(
          file_content_store_, config.index.max_file_size)) {
    snapshot_.store(std::make_shared<const FileSnapshot>());
}

MasterIndex::~MasterIndex() {
    clear();
}

// -- Lock-free snapshot -------------------------------------------------------

std::shared_ptr<const FileSnapshot> MasterIndex::load_snapshot() const {
    return snapshot_.load(std::memory_order_acquire);
}

std::shared_ptr<const FileSnapshot> MasterIndex::read_snapshot() const {
    return load_snapshot();
}

FileID MasterIndex::path_to_id(const std::string& path) const {
    auto snap = load_snapshot();
    auto it = snap->file_map.find(path);
    return it != snap->file_map.end() ? it->second : FileID{0};
}

std::string_view MasterIndex::id_to_path(const FileSnapshot& snap,
                                         FileID file_id) const {
    auto it = snap.reverse_file_map.find(file_id);
    if (it == snap.reverse_file_map.end()) return {};
    return it->second;
}

std::string MasterIndex::id_to_path(FileID file_id) const {
    auto snap = load_snapshot();
    return std::string(id_to_path(*snap, file_id));
}

// -- Bulk indexing mode toggle ------------------------------------------------

void MasterIndex::set_bulk_indexing(bool enabled) {
    trigram_index_.set_bulk_indexing(enabled);
    // Opens/closes the ReferenceTracker bulk RCU window (staging snapshot +
    // single publish on close), avoiding O(files^2) per-file snapshot clones.
    ref_tracker_.set_bulk_indexing(enabled);
    symbol_location_index_.set_bulk_indexing(enabled);
    postings_index_.set_bulk_indexing(enabled);
}

// -- Directory indexing -------------------------------------------------------

bool MasterIndex::index_directory(const std::string& root) {
    if (root.empty()) return false;
    if (!std::filesystem::exists(root)) return false;

    std::lock_guard<std::mutex> bulk_lock(bulk_mu_);

    // Clear-and-claim under stop_mu_: a concurrent `request_stop()` either
    // runs entirely before this block (its stale flag is overwritten —
    // intended, no run was in flight) or entirely after (it observes
    // is_indexing_=1 and either forwards into the now-published pipeline
    // or sets stop_requested_=true for the second-stage check below).
    {
        std::lock_guard<std::mutex> stop_lock(stop_mu_);
        int32_t expected = 0;
        if (!is_indexing_.compare_exchange_strong(expected, 1)) {
            return false;
        }
        stop_requested_.store(false, std::memory_order_release);
    }

    auto start = std::chrono::steady_clock::now();

    // Clear existing data for a full reindex.
    trigram_index_.clear();
    ref_tracker_.clear();
    postings_index_.clear();
    symbol_location_index_.clear();
    file_content_store_->clear();
    processed_files_.store(0, std::memory_order_release);
    total_files_.store(0, std::memory_order_release);

    set_bulk_indexing(true);

    // Run the pipeline.
    Pipeline pipeline(config_, file_service_,
                      &trigram_index_, &ref_tracker_, &postings_index_);

    // Wire the integrator with auxiliary indexes so symbol-aware paths
    // (import resolution, symbol location index) are populated as
    // ProcessedFile results stream through. Without this, `merge_symbols`
    // skips imports and the symbol location index stays empty.
    pipeline.integrator().set_file_content_store(file_content_store_.get());
    pipeline.integrator().set_symbol_location_index(&symbol_location_index_);

    // Publish the active pipeline so `request_stop()` can forward into
    // it from another thread. Re-check stop_requested_ under the lock
    // to avoid losing a stop request that arrived between the CAS above
    // and the pipeline construction.
    {
        std::lock_guard<std::mutex> stop_lock(stop_mu_);
        active_pipeline_ = &pipeline;
        if (stop_requested_.load(std::memory_order_acquire)) {
            pipeline.request_stop();
        }
    }

    pipeline.run();

    {
        std::lock_guard<std::mutex> stop_lock(stop_mu_);
        active_pipeline_ = nullptr;
    }

    // Publish the symbol-location index now: the pipeline has fully populated
    // it, and process_all_references reads it (find_symbol_id_at_position) to
    // resolve reference sources/targets. Under the bulk window those writes sat
    // in the staging snapshot, invisible to the lock-free reader; close the
    // window here so resolution sees the committed data. (No further
    // index_file_symbols runs in this pass; the redundant close at
    // set_bulk_indexing(false) below is a no-op.)
    symbol_location_index_.set_bulk_indexing(false);

    // Post-pipeline: resolve cross-file references.
    ref_tracker_.process_all_references();

    // Build the file snapshot from the integrator's file map.
    auto& integrator = pipeline.integrator();
    auto& progress_tracker = pipeline.progress_tracker();
    auto new_snapshot = std::make_shared<FileSnapshot>();

    int integrated = progress_tracker.integrated_count();
    int total_ids = integrator.file_count();
    int scan_limit = (total_ids > integrated ? total_ids * 2 : integrated);
    if (scan_limit < 1) scan_limit = 1;

    for (int i = 1; i <= scan_limit; ++i) {
        FileID fid{static_cast<uint32_t>(i)};
        const auto& p = integrator.id_to_path(fid);
        if (!p.empty()) {
            new_snapshot->file_map[p] = fid;
            new_snapshot->reverse_file_map[fid] = p;
        }
    }

    auto progress = pipeline.get_progress();
    snapshot_.store(std::move(new_snapshot), std::memory_order_release);
    processed_files_.store(static_cast<int64_t>(integrated),
                           std::memory_order_release);
    total_files_.store(static_cast<int64_t>(progress.total_files),
                       std::memory_order_release);

    set_bulk_indexing(false);

    auto elapsed = std::chrono::steady_clock::now() - start;
    indexing_time_ns_.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count(),
        std::memory_order_release);

    is_indexing_.store(0, std::memory_order_release);
    return true;
}

void MasterIndex::request_stop() {
    // Set the persistent flag first so an `index_directory` call that
    // hasn't yet published its pipeline pointer still observes the stop
    // request when it acquires `stop_mu_`.
    stop_requested_.store(true, std::memory_order_release);

    std::lock_guard<std::mutex> stop_lock(stop_mu_);
    if (active_pipeline_ != nullptr) {
        active_pipeline_->request_stop();
    }
}

bool MasterIndex::stop_requested() const {
    return stop_requested_.load(std::memory_order_acquire);
}

MasterIndex::IndexingProgressSnapshot MasterIndex::get_progress() const {
    IndexingProgressSnapshot snap{};

    // Fast path: no active run -> idle, zero counters. We check
    // is_indexing_ first to avoid a needless mutex acquisition on the
    // common /status read path. The active_pipeline_ pointer is only
    // valid while is_indexing_ is set, but we still take stop_mu_ before
    // dereferencing it: a racing index_directory() return clears
    // is_indexing_ and the pipeline pointer, and the lock provides the
    // happens-before edge guaranteeing we see a consistent state.
    if (is_indexing_.load(std::memory_order_acquire) == 0) {
        return snap;
    }

    std::lock_guard<std::mutex> lock(stop_mu_);
    if (active_pipeline_ == nullptr) {
        // Race: the run finished between the is_indexing_ check and the
        // lock. Report idle rather than reading a stale tracker — the
        // pipeline lives on the index_directory() stack frame and its
        // ProgressTracker may be torn down once the lock is released
        // again.
        return snap;
    }

    auto pipeline_progress = active_pipeline_->get_progress();

    // Phase derivation.
    //
    // The ProgressTracker exposes is_scanning (1 while scan_directory
    // walks the tree, 0 once set_total has flipped it). After scanning
    // completes the Processor stage drains files; once
    // processed >= total we treat the residual time as "merging" — the
    // Integrator is finishing posting-list flushes and the symbol/ref
    // joins. This three-way derivation (scanning vs indexing vs merging)
    // matches how the JSON consumer reasons about user-visible progress.
    if (pipeline_progress.is_scanning) {
        snap.phase = IndexingPhase::Scanning;
    } else if (pipeline_progress.total_files == 0 ||
               pipeline_progress.files_processed >=
                   pipeline_progress.total_files) {
        // Empty corpus (total==0 after set_total) and post-process
        // (processed>=total) both surface as Merging — the Pipeline is
        // wrapping up its final stages and there is no per-file
        // progress left for the user to observe.
        snap.phase = IndexingPhase::Merging;
    } else {
        snap.phase = IndexingPhase::Indexing;
    }

    // The acceptance JSON uses "files_scanned" for the live counter.
    // Map to ProgressTracker semantics:
    //   - During scan: surface the Scanner's live scanned-files counter
    //     so /status polling shows discovery moving even before
    //     set_total flips the phase. files_total stays 0 until the
    //     scanner finishes (we don't know the final count yet), which
    //     is the documented "scanning" semantics.
    //   - After scan (indexing/merging): surface files_processed since
    //     the scanner counter is frozen at the total and the user wants
    //     to see the Processor stage advancing.
    if (pipeline_progress.is_scanning) {
        snap.files_scanned = pipeline_progress.files_scanned;
    } else {
        snap.files_scanned = pipeline_progress.files_processed;
    }
    snap.files_total = pipeline_progress.total_files;

    // Percent-complete: clamp to [0, 100]. During scanning we expose 0
    // (we don't know total yet); during indexing it's processed/total;
    // during merging it's pinned at 100 because every file has been
    // processed and the residual work is index-side bookkeeping that
    // the user shouldn't interpret as additional file progress.
    if (snap.phase == IndexingPhase::Scanning) {
        snap.percent_complete = 0;
    } else if (snap.phase == IndexingPhase::Merging) {
        snap.percent_complete = 100;
    } else if (snap.files_total > 0) {
        int pct = static_cast<int>(
            (static_cast<int64_t>(snap.files_scanned) * 100) /
            snap.files_total);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        snap.percent_complete = pct;
    } else {
        snap.percent_complete = 0;
    }

    snap.elapsed_ms = pipeline_progress.elapsed.count();
    return snap;
}

// -- Single-file operations ---------------------------------------------------

bool MasterIndex::index_file(const std::string& path) {
    if (path.empty()) return false;
    if (!std::filesystem::exists(path)) return false;

    // Load content through FileService.
    auto result = file_service_->load_file_from_disk(path);
    if (!result) return false;
    FileID file_id = result.value();

    auto content_sv = file_content_store_->get_content(file_id);
    if (content_sv.empty()) return false;

    // snapshot_mu_ serializes the whole single-file composite write (per-index
    // publish + file-snapshot swap) as a unit, so a concurrent reader never
    // sees a file present in one index but missing from the file map. Reads are
    // lock-free (RCU snapshots); the old IndexLockManager is retired — its
    // ReadGuards had no remaining callers after execute_search went lock-free
    // (01KSWHQ742), and its WriteGuards only duplicated this mutex.
    std::lock_guard<std::mutex> snap_lock(snapshot_mu_);

    trigram_index_.index_file(file_id, content_sv);
    postings_index_.index_file(file_id, content_sv);
    update_snapshot_for_file(path, file_id, FileID{0}, false);

    processed_files_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool MasterIndex::update_file(const std::string& path, std::string_view content) {
    if (path.empty() || content.empty()) return false;
    if (!std::filesystem::exists(path)) return false;

    if (static_cast<int64_t>(content.size()) > config_.index.max_file_size) {
        return false;
    }

    // snapshot_mu_ serializes the whole composite write (see index_file).
    // IndexLockManager retired — RCU reads need no lock; these WriteGuards only
    // duplicated this mutex.
    std::lock_guard<std::mutex> snap_lock(snapshot_mu_);

    auto snap = load_snapshot();
    auto it = snap->file_map.find(path);
    FileID old_id{0};
    bool existed = false;
    if (it != snap->file_map.end()) {
        old_id = it->second;
        existed = true;
        remove_file_from_indexes(old_id, path);
    }

    FileID new_id = file_content_store_->load_file(path, content);
    if (new_id == FileID{0}) return false;

    trigram_index_.index_file(new_id, content);
    postings_index_.index_file(new_id, content);

    update_snapshot_for_file(path, new_id, old_id, existed);
    return true;
}

bool MasterIndex::remove_file(const std::string& path) {
    // snapshot_mu_ serializes the whole composite write and spans the file_map
    // lookup so two concurrent removals can't both act on the same id. RCU
    // reads need no lock; the old IndexLockManager WriteGuards are retired.
    std::lock_guard<std::mutex> snap_lock(snapshot_mu_);

    auto snap = load_snapshot();
    auto it = snap->file_map.find(path);
    if (it == snap->file_map.end()) return true;

    FileID file_id = it->second;

    remove_file_from_indexes(file_id, path);
    file_content_store_->invalidate_file(path);

    auto new_snap = std::make_shared<FileSnapshot>(*snap);
    new_snap->file_map.erase(path);
    new_snap->reverse_file_map.erase(file_id);
    snapshot_.store(std::move(new_snap), std::memory_order_release);

    processed_files_.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

// -- Lifecycle ----------------------------------------------------------------

bool MasterIndex::clear() {
    if (is_indexing_.load(std::memory_order_acquire) != 0) {
        return false;
    }

    trigram_index_.clear();
    ref_tracker_.clear();
    postings_index_.clear();
    symbol_location_index_.clear();
    file_content_store_->clear();

    {
        std::lock_guard<std::mutex> lock(snapshot_mu_);
        snapshot_.store(std::make_shared<const FileSnapshot>(),
                        std::memory_order_release);
    }

    processed_files_.store(0, std::memory_order_release);
    total_files_.store(0, std::memory_order_release);
    indexing_time_ns_.store(0, std::memory_order_release);

    return true;
}

// -- Statistics ---------------------------------------------------------------

MasterIndexStats MasterIndex::get_stats() const {
    MasterIndexStats stats;
    stats.total_files = file_count();
    stats.total_symbols = ref_tracker_.get_reference_stats().total_symbols;
    stats.total_references = ref_tracker_.get_reference_stats().total_references;
    stats.indexing_time_ns = indexing_time_ns_.load(std::memory_order_acquire);
    stats.is_indexing = is_indexing_.load(std::memory_order_acquire) != 0;
    stats.processed_files = processed_files_.load(std::memory_order_acquire);
    stats.total_files_to_process = total_files_.load(std::memory_order_acquire);
    return stats;
}

int MasterIndex::file_count() const {
    return load_snapshot()->file_count();
}

bool MasterIndex::is_indexing() const {
    return is_indexing_.load(std::memory_order_acquire) != 0;
}

// -- Sub-index access ---------------------------------------------------------

TrigramIndex& MasterIndex::trigram_index() { return trigram_index_; }
const TrigramIndex& MasterIndex::trigram_index() const { return trigram_index_; }

ReferenceTracker& MasterIndex::ref_tracker() { return ref_tracker_; }
const ReferenceTracker& MasterIndex::ref_tracker() const { return ref_tracker_; }

PostingsIndex& MasterIndex::postings_index() { return postings_index_; }
const PostingsIndex& MasterIndex::postings_index() const { return postings_index_; }

SymbolLocationIndex& MasterIndex::symbol_location_index() {
    return symbol_location_index_;
}
const SymbolLocationIndex& MasterIndex::symbol_location_index() const {
    return symbol_location_index_;
}

FileContentStore& MasterIndex::file_content_store() {
    return *file_content_store_;
}
const FileContentStore& MasterIndex::file_content_store() const {
    return *file_content_store_;
}

std::shared_ptr<FileContentStore> MasterIndex::file_content_store_ptr() {
    return file_content_store_;
}

const Config& MasterIndex::config() const { return config_; }

// -- Private helpers ----------------------------------------------------------

void MasterIndex::update_snapshot_for_file(const std::string& path,
                                            FileID new_id, FileID old_id,
                                            bool existed) {
    auto old_snap = load_snapshot();
    auto new_snap = std::make_shared<FileSnapshot>(*old_snap);

    if (existed) {
        new_snap->file_map.erase(path);
        new_snap->reverse_file_map.erase(old_id);
    }

    new_snap->file_map[path] = new_id;
    new_snap->reverse_file_map[new_id] = path;

    snapshot_.store(std::move(new_snap), std::memory_order_release);
}

void MasterIndex::remove_file_from_indexes(FileID file_id,
                                            const std::string& /*path*/) {
    trigram_index_.remove_file(file_id);
    postings_index_.remove_file(file_id);
    ref_tracker_.remove_file(file_id);
    symbol_location_index_.remove_file(file_id);
}

}  // namespace lci
