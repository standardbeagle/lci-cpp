#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace lci {

/// Index type identifier for fine-grained locking.
enum class IndexType : uint8_t {
    Trigram = 0,
    Symbol,
    Reference,
    Postings,
    Location,
    Content,
};

/// Returns a human-readable name for an IndexType.
const char* index_type_name(IndexType type);

/// Configuration for the IndexLockManager.
struct IndexLockConfig {
    std::chrono::milliseconds default_read_timeout{5000};
    std::chrono::milliseconds default_write_timeout{30000};
    int max_retry_attempts{3};
    double retry_backoff_factor{2.0};
};

/// Creates a default IndexLockConfig.
IndexLockConfig make_default_lock_config();

/// Fine-grained lock manager for index operations.
///
/// Provides per-index-type read/write locking with retry and timeout support.
/// Locks are acquired in a consistent order (by IndexType ordinal) to prevent
/// deadlocks when multiple locks are needed.
class IndexLockManager {
  public:
    explicit IndexLockManager(IndexLockConfig config = make_default_lock_config());

    /// Acquires a read lock for a single index type.
    /// Returns true on success, false on timeout after retries.
    [[nodiscard]] bool acquire_read(IndexType type);

    /// Releases a read lock for a single index type.
    void release_read(IndexType type);

    /// Acquires a write lock for a single index type.
    /// Returns true on success, false on timeout after retries.
    [[nodiscard]] bool acquire_write(IndexType type);

    /// Releases a write lock for a single index type.
    void release_write(IndexType type);

    /// RAII guard for a read lock on a single index type.
    class ReadGuard {
      public:
        ReadGuard(IndexLockManager& mgr, IndexType type);
        ~ReadGuard();
        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;
        bool locked() const { return locked_; }
      private:
        IndexLockManager& mgr_;
        IndexType type_;
        bool locked_;
    };

    /// RAII guard for a write lock on a single index type.
    class WriteGuard {
      public:
        WriteGuard(IndexLockManager& mgr, IndexType type);
        ~WriteGuard();
        WriteGuard(const WriteGuard&) = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;
        bool locked() const { return locked_; }
      private:
        IndexLockManager& mgr_;
        IndexType type_;
        bool locked_;
    };

  private:
    static constexpr int kIndexTypeCount = 6;

    IndexLockConfig config_;
    std::shared_timed_mutex locks_[kIndexTypeCount];

    int type_index(IndexType type) const;
};

}  // namespace lci
