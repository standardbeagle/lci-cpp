#include <lci/string_pool.h>

#include <algorithm>
#include <limits>

namespace lci {

StringPool::StringPool() = default;

uint32_t StringPool::intern(std::string_view s) {
    // Fast path: check if already interned under shared lock.
    {
        std::shared_lock lock(mu_);
        std::string key(s);
        auto it = lookup_.find(key);
        if (it != lookup_.end()) return it->second;
    }

    // Slow path: acquire exclusive lock and insert.
    std::unique_lock lock(mu_);

    // Double-check after acquiring write lock.
    std::string key(s);
    auto it = lookup_.find(key);
    if (it != lookup_.end()) return it->second;

    uint32_t id = ++next_id_;
    strings_.emplace(id, key);
    lookup_.emplace(std::move(key), id);
    return id;
}

StringRange StringPool::intern_range(std::string_view s) {
    uint32_t id = intern(s);
    return {id, 0, static_cast<uint32_t>(s.size())};
}

std::pair<std::string_view, bool> StringPool::get_string(uint32_t id) const {
    std::shared_lock lock(mu_);
    auto it = strings_.find(id);
    if (it == strings_.end()) return {{}, false};
    return {it->second, true};
}

std::pair<std::string_view, bool> StringPool::get_range_string(const StringRange& r) const {
    std::shared_lock lock(mu_);
    auto it = strings_.find(r.pool_id);
    if (it == strings_.end()) return {{}, false};

    const auto& s = it->second;
    if (r.start >= s.size()) return {{}, false};

    // Clamp length against the remaining bytes without computing r.start +
    // r.length, which can wrap uint32 and defeat an `end > size` check (same
    // class as the get_string overflow fixed in FileContentStore).
    uint32_t max_len = static_cast<uint32_t>(s.size() - r.start);
    uint32_t len = r.length > max_len ? max_len : r.length;

    return {std::string_view(s).substr(r.start, len), true};
}

StringRange StringPool::create_subrange(const StringRange& parent,
                                         uint32_t start, uint32_t length) {
    // Saturate the offset add so an overflow produces an out-of-range start
    // (yielding an empty slice at read time) rather than wrapping to a small
    // in-range offset that would return the wrong bytes.
    uint64_t abs_start = static_cast<uint64_t>(parent.start) + start;
    uint32_t clamped = abs_start > std::numeric_limits<uint32_t>::max()
                           ? std::numeric_limits<uint32_t>::max()
                           : static_cast<uint32_t>(abs_start);
    return {parent.pool_id, clamped, length};
}

size_t StringPool::size() const {
    std::shared_lock lock(mu_);
    return strings_.size();
}

// -- FileStringPool -----------------------------------------------------------

FileStringPool::FileStringPool(StringPool& pool, std::string_view content)
    : file_id_(pool.intern(content)) {
    uint32_t start = 0;
    for (uint32_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n') {
            line_ranges_.push_back({file_id_, start, i - start});
            start = i + 1;
        }
    }
    if (start < content.size()) {
        line_ranges_.push_back({file_id_, start,
                                static_cast<uint32_t>(content.size()) - start});
    }
}

std::pair<StringRange, bool> FileStringPool::get_line(int line_num) const {
    if (line_num < 0 || line_num >= static_cast<int>(line_ranges_.size())) {
        return {{}, false};
    }
    return {line_ranges_[static_cast<size_t>(line_num)], true};
}

std::vector<StringRange> FileStringPool::get_lines(int start, int end) const {
    start = std::max(start, 0);
    end = std::min(end, static_cast<int>(line_ranges_.size()));
    if (start >= end) return {};
    return {line_ranges_.begin() + start, line_ranges_.begin() + end};
}

int FileStringPool::line_count() const {
    return static_cast<int>(line_ranges_.size());
}

std::vector<StringRange> FileStringPool::get_context_lines(int line_num,
                                                            int context_before,
                                                            int context_after) const {
    return get_lines(line_num - context_before, line_num + context_after + 1);
}

}  // namespace lci
