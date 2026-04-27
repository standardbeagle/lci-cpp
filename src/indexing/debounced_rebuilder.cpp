#include <lci/indexing/debounced_rebuilder.h>

namespace lci {

DebouncedRebuilder::DebouncedRebuilder(std::chrono::milliseconds debounce)
    : debounce_(debounce) {
    timer_thread_ = std::thread(&DebouncedRebuilder::timer_thread_func, this);
}

DebouncedRebuilder::~DebouncedRebuilder() {
    shutdown();
}

void DebouncedRebuilder::set_callback(RebuildCallback cb) {
    std::lock_guard lock(mu_);
    callback_ = std::move(cb);
}

void DebouncedRebuilder::schedule_rebuild(FileID file_id) {
    std::lock_guard lock(mu_);
    pending_.insert(file_id);
    deadline_ = std::chrono::steady_clock::now() + debounce_;
    has_pending_ = true;
    cv_.notify_one();
}

void DebouncedRebuilder::force_rebuild() {
    flush_pending();
}

int DebouncedRebuilder::pending_count() const {
    std::lock_guard lock(mu_);
    return static_cast<int>(pending_.size());
}

void DebouncedRebuilder::set_debounce(std::chrono::milliseconds ms) {
    std::lock_guard lock(mu_);
    debounce_ = ms;
}

void DebouncedRebuilder::shutdown() {
    bool was_running = running_.exchange(false);
    if (!was_running) return;

    cv_.notify_one();
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
}

void DebouncedRebuilder::timer_thread_func() {
    std::unique_lock lock(mu_);
    while (running_.load(std::memory_order_relaxed)) {
        if (!has_pending_) {
            cv_.wait(lock, [this] {
                return has_pending_ || !running_.load(std::memory_order_relaxed);
            });
            if (!running_.load(std::memory_order_relaxed)) return;
        }

        auto now = std::chrono::steady_clock::now();
        if (now < deadline_) {
            cv_.wait_until(lock, deadline_);
            continue;
        }

        // Deadline reached -- collect pending files and fire callback.
        has_pending_ = false;
        std::vector<FileID> files(pending_.begin(), pending_.end());
        pending_.clear();

        auto cb = callback_;
        lock.unlock();

        if (cb && !files.empty()) {
            cb(files);
        }

        lock.lock();
    }
}

void DebouncedRebuilder::flush_pending() {
    std::vector<FileID> files;
    RebuildCallback cb;
    {
        std::lock_guard lock(mu_);
        files.assign(pending_.begin(), pending_.end());
        pending_.clear();
        has_pending_ = false;
        cb = callback_;
    }
    if (cb && !files.empty()) {
        cb(files);
    }
}

}  // namespace lci
