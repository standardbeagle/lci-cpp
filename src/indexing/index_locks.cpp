#include <lci/indexing/index_locks.h>

#include <thread>

namespace lci {

const char* index_type_name(IndexType type) {
    switch (type) {
        case IndexType::Trigram:   return "Trigram";
        case IndexType::Symbol:    return "Symbol";
        case IndexType::Reference: return "Reference";
        case IndexType::Postings:  return "Postings";
        case IndexType::Location:  return "Location";
        case IndexType::Content:   return "Content";
    }
    return "Unknown";
}

IndexLockConfig make_default_lock_config() {
    return IndexLockConfig{};
}

IndexLockManager::IndexLockManager(IndexLockConfig config)
    : config_(config) {}

int IndexLockManager::type_index(IndexType type) const {
    return static_cast<int>(type);
}

bool IndexLockManager::acquire_read(IndexType type) {
    int idx = type_index(type);
    auto backoff = std::chrono::milliseconds(1);
    for (int attempt = 0; attempt < config_.max_retry_attempts; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(backoff);
            backoff = std::chrono::milliseconds(
                static_cast<int64_t>(
                    static_cast<double>(backoff.count()) *
                    config_.retry_backoff_factor));
        }
        if (locks_[idx].try_lock_shared_for(config_.default_read_timeout)) {
            return true;
        }
    }
    return false;
}

void IndexLockManager::release_read(IndexType type) {
    locks_[type_index(type)].unlock_shared();
}

bool IndexLockManager::acquire_write(IndexType type) {
    int idx = type_index(type);
    auto backoff = std::chrono::milliseconds(1);
    for (int attempt = 0; attempt < config_.max_retry_attempts; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(backoff);
            backoff = std::chrono::milliseconds(
                static_cast<int64_t>(
                    static_cast<double>(backoff.count()) *
                    config_.retry_backoff_factor));
        }
        if (locks_[idx].try_lock_for(config_.default_write_timeout)) {
            return true;
        }
    }
    return false;
}

void IndexLockManager::release_write(IndexType type) {
    locks_[type_index(type)].unlock();
}

// -- ReadGuard ----------------------------------------------------------------

IndexLockManager::ReadGuard::ReadGuard(IndexLockManager& mgr, IndexType type)
    : mgr_(mgr), type_(type), locked_(mgr.acquire_read(type)) {}

IndexLockManager::ReadGuard::~ReadGuard() {
    if (locked_) mgr_.release_read(type_);
}

// -- WriteGuard ---------------------------------------------------------------

IndexLockManager::WriteGuard::WriteGuard(IndexLockManager& mgr, IndexType type)
    : mgr_(mgr), type_(type), locked_(mgr.acquire_write(type)) {}

IndexLockManager::WriteGuard::~WriteGuard() {
    if (locked_) mgr_.release_write(type_);
}

}  // namespace lci
