#include <lci/indexing/deleted_file_tracker.h>

namespace lci {

DeletedFileTracker::DeletedFileTracker() {
    snapshot_.store(std::make_shared<const DeletedFileSet>());
}

std::shared_ptr<const DeletedFileSet> DeletedFileTracker::load() const {
    return snapshot_.load(std::memory_order_acquire);
}

void DeletedFileTracker::mark_deleted(FileID file_id) {
    for (;;) {
        auto old_set = load();
        if (old_set->contains(file_id)) return;

        auto new_set = std::make_shared<DeletedFileSet>(*old_set);
        new_set->files.insert(file_id);
        auto expected = old_set;
        if (snapshot_.compare_exchange_weak(
                expected,
                std::move(new_set),
                std::memory_order_release,
                std::memory_order_relaxed)) {
            return;
        }
    }
}

void DeletedFileTracker::mark_deleted_batch(
    const std::vector<FileID>& file_ids) {
    if (file_ids.empty()) return;

    for (;;) {
        auto old_set = load();

        int new_count = 0;
        for (FileID id : file_ids) {
            if (!old_set->contains(id)) ++new_count;
        }
        if (new_count == 0) return;

        auto new_set = std::make_shared<DeletedFileSet>(*old_set);
        new_set->files.reserve(old_set->files.size() +
                               static_cast<size_t>(new_count));
        for (FileID id : file_ids) {
            new_set->files.insert(id);
        }

        auto expected = old_set;
        if (snapshot_.compare_exchange_weak(
                expected,
                std::move(new_set),
                std::memory_order_release,
                std::memory_order_relaxed)) {
            return;
        }
    }
}

bool DeletedFileTracker::is_deleted(FileID file_id) const {
    return load()->contains(file_id);
}

std::vector<FileID> DeletedFileTracker::filter_candidates(
    const std::vector<FileID>& candidates) const {
    auto snap = load();
    if (snap->files.empty()) return candidates;

    std::vector<FileID> result;
    result.reserve(candidates.size());
    for (FileID id : candidates) {
        if (!snap->contains(id)) {
            result.push_back(id);
        }
    }
    return result;
}

void DeletedFileTracker::clear() {
    snapshot_.store(std::make_shared<const DeletedFileSet>(),
                    std::memory_order_release);
}

int DeletedFileTracker::deleted_count() const {
    return load()->size();
}

std::vector<FileID> DeletedFileTracker::deleted_file_ids() const {
    auto snap = load();
    std::vector<FileID> result;
    result.reserve(snap->files.size());
    for (FileID id : snap->files) {
        result.push_back(id);
    }
    return result;
}

}  // namespace lci
