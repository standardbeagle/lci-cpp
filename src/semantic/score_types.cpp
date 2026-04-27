#include <lci/semantic/score_types.h>

namespace lci {

// -- LRUCache -----------------------------------------------------------------

LRUCache::LRUCache(int max_size)
    : max_size_(max_size > 0 ? max_size : 100) {}

const NormalizedQuery* LRUCache::get(const std::string& key) {
    std::lock_guard lock(mu_);
    auto it = items_.find(key);
    if (it == items_.end()) return nullptr;

    // Move to front (recently used).
    order_.splice(order_.begin(), order_, it->second);
    return &it->second->value;
}

void LRUCache::set(const std::string& key, NormalizedQuery value) {
    std::lock_guard lock(mu_);

    auto it = items_.find(key);
    if (it != items_.end()) {
        // Update existing and move to front.
        it->second->value = std::move(value);
        order_.splice(order_.begin(), order_, it->second);
        return;
    }

    // Insert new entry at front.
    order_.push_front(Entry{key, std::move(value)});
    items_[key] = order_.begin();

    // Evict oldest if over capacity.
    if (static_cast<int>(order_.size()) > max_size_) {
        auto& oldest = order_.back();
        items_.erase(oldest.key);
        order_.pop_back();
    }
}

void LRUCache::clear() {
    std::lock_guard lock(mu_);
    items_.clear();
    order_.clear();
}

int LRUCache::size() const {
    std::lock_guard lock(mu_);
    return static_cast<int>(order_.size());
}

}  // namespace lci
