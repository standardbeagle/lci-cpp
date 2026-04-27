#include <lci/indexing/master_index.h>

#include <chrono>
#include <filesystem>

namespace lci {

// -- Helpers ------------------------------------------------------------------

static const std::string kEmptyString;

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

std::string MasterIndex::id_to_path(FileID file_id) const {
    auto snap = load_snapshot();
    auto it = snap->reverse_file_map.find(file_id);
    return it != snap->reverse_file_map.end() ? it->second : kEmptyString;
}

// -- Bulk indexing mode toggle ------------------------------------------------

void MasterIndex::set_bulk_indexing(bool enabled) {
    int32_t v = enabled ? 1 : 0;
    trigram_index_.set_bulk_indexing(enabled);
    ref_tracker_.bulk_indexing.store(v, std::memory_order_release);
    postings_index_.bulk_indexing.store(v, std::memory_order_release);
}

// -- Directory indexing -------------------------------------------------------

bool MasterIndex::index_directory(const std::string& root) {
    if (root.empty()) return false;
    if (!std::filesystem::exists(root)) return false;

    std::lock_guard<std::mutex> bulk_lock(bulk_mu_);

    int32_t expected = 0;
    if (!is_indexing_.compare_exchange_strong(expected, 1)) {
        return false;
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
    pipeline.run();

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

    // Acquire write locks for all index types.
    IndexLockManager::WriteGuard trigram_lock(lock_manager_, IndexType::Trigram);
    IndexLockManager::WriteGuard symbol_lock(lock_manager_, IndexType::Symbol);
    IndexLockManager::WriteGuard ref_lock(lock_manager_, IndexType::Reference);
    IndexLockManager::WriteGuard postings_lock(lock_manager_, IndexType::Postings);
    IndexLockManager::WriteGuard location_lock(lock_manager_, IndexType::Location);
    IndexLockManager::WriteGuard content_lock(lock_manager_, IndexType::Content);

    if (!trigram_lock.locked() || !symbol_lock.locked() ||
        !ref_lock.locked() || !postings_lock.locked() ||
        !location_lock.locked() || !content_lock.locked()) {
        return false;
    }

    trigram_index_.index_file(file_id, content_sv);
    postings_index_.index_file(file_id, content_sv);

    std::lock_guard<std::mutex> snap_lock(snapshot_mu_);
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

    // Acquire write locks for all index types.
    IndexLockManager::WriteGuard trigram_lock(lock_manager_, IndexType::Trigram);
    IndexLockManager::WriteGuard symbol_lock(lock_manager_, IndexType::Symbol);
    IndexLockManager::WriteGuard ref_lock(lock_manager_, IndexType::Reference);
    IndexLockManager::WriteGuard postings_lock(lock_manager_, IndexType::Postings);
    IndexLockManager::WriteGuard location_lock(lock_manager_, IndexType::Location);
    IndexLockManager::WriteGuard content_lock(lock_manager_, IndexType::Content);

    if (!trigram_lock.locked() || !symbol_lock.locked() ||
        !ref_lock.locked() || !postings_lock.locked() ||
        !location_lock.locked() || !content_lock.locked()) {
        return false;
    }

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
    IndexLockManager::WriteGuard trigram_lock(lock_manager_, IndexType::Trigram);
    IndexLockManager::WriteGuard symbol_lock(lock_manager_, IndexType::Symbol);
    IndexLockManager::WriteGuard ref_lock(lock_manager_, IndexType::Reference);
    IndexLockManager::WriteGuard postings_lock(lock_manager_, IndexType::Postings);
    IndexLockManager::WriteGuard location_lock(lock_manager_, IndexType::Location);
    IndexLockManager::WriteGuard content_lock(lock_manager_, IndexType::Content);

    if (!trigram_lock.locked() || !symbol_lock.locked() ||
        !ref_lock.locked() || !postings_lock.locked() ||
        !location_lock.locked() || !content_lock.locked()) {
        return false;
    }

    auto snap = load_snapshot();
    auto it = snap->file_map.find(path);
    if (it == snap->file_map.end()) return true;

    FileID file_id = it->second;

    std::lock_guard<std::mutex> snap_lock(snapshot_mu_);

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
