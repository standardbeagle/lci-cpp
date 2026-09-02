#include <lci/indexing/debounced_rebuilder.h>

#include <algorithm>

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
    {
        std::lock_guard lock(mu_);
        if (!running_) return;
        running_ = false;
        has_pending_ = false;
    }

    cv_.notify_all();
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
}

void DebouncedRebuilder::timer_thread_func() {
    std::unique_lock lock(mu_);
    while (running_) {
        if (!has_pending_) {
            cv_.wait(lock, [this] {
                return has_pending_ || !running_;
            });
            if (!running_) return;
        }

        auto now = std::chrono::steady_clock::now();
        if (now < deadline_) {
            cv_.wait_until(lock, deadline_, [this] {
                return !running_ || !has_pending_ ||
                       std::chrono::steady_clock::now() >= deadline_;
            });
            continue;
        }

        // Deadline reached -- collect pending files and fire callback.
        // pending_ is a hash set: its iteration order is randomized per
        // process, and the callback's file order reaches user-visible rebuild
        // output. Sort so a given pending batch always rebuilds in file-id
        // order.
        has_pending_ = false;
        std::vector<FileID> files(pending_.begin(), pending_.end());
        std::sort(files.begin(), files.end());
        pending_.clear();

        auto cb = callback_;
        // Take the dispatch lock BEFORE releasing mu_ (consistent
        // mu_ -> dispatch_mu_ order with flush_pending), so a concurrent
        // flush can neither run its callback at the same time as this one
        // nor overtake this earlier-gathered batch.
        std::unique_lock dispatch(dispatch_mu_);
        lock.unlock();

        if (cb && !files.empty()) {
            cb(files);
        }

        dispatch.unlock();
        lock.lock();
    }
}

void DebouncedRebuilder::flush_pending() {
    std::vector<FileID> files;
    RebuildCallback cb;
    std::unique_lock lock(mu_);
    files.assign(pending_.begin(), pending_.end());
    std::sort(files.begin(), files.end());
    pending_.clear();
    has_pending_ = false;
    cb = callback_;
    // mu_ -> dispatch_mu_ order matches the timer thread; holding
    // dispatch_mu_ across the callback serializes invocation with it.
    std::unique_lock dispatch(dispatch_mu_);
    lock.unlock();
    cv_.notify_all();
    if (cb && !files.empty()) {
        cb(files);
    }
}

}  // namespace lci
