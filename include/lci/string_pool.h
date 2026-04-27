#pragma once

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace lci {

/// Range within a pooled string, identified by pool ID plus byte offset and length.
struct StringRange {
    uint32_t pool_id{};
    uint32_t start{};
    uint32_t length{};
};

/// Thread-safe interned string storage with deduplication.
/// Concurrent reads are lock-free relative to each other (shared_mutex).
/// Strings are stored once and referenced by a uint32_t pool ID.
class StringPool {
  public:
    StringPool();

    /// Interns a string, returning its pool ID.
    /// If the string already exists, returns the existing ID.
    uint32_t intern(std::string_view s);

    /// Interns a string and returns a StringRange covering the full string.
    StringRange intern_range(std::string_view s);

    /// Retrieves the full string for a given pool ID.
    /// Returns empty string_view and false if the ID is not found.
    std::pair<std::string_view, bool> get_string(uint32_t id) const;

    /// Retrieves the substring specified by a StringRange.
    /// Returns empty string_view and false if the range is invalid.
    std::pair<std::string_view, bool> get_range_string(const StringRange& r) const;

    /// Creates a sub-range within an existing range.
    static StringRange create_subrange(const StringRange& parent,
                                       uint32_t start, uint32_t length);

    /// Returns the number of unique strings in the pool.
    size_t size() const;

  private:
    mutable std::shared_mutex mu_;
    absl::flat_hash_map<uint32_t, std::string> strings_;
    absl::flat_hash_map<std::string, uint32_t> lookup_;
    uint32_t next_id_{0};
};

/// Manages strings for a specific file with pre-computed line ranges.
class FileStringPool {
  public:
    /// Constructs a file pool, interning the content and computing line ranges.
    FileStringPool(StringPool& pool, std::string_view content);

    /// Returns the StringRange for a specific line (0-indexed).
    std::pair<StringRange, bool> get_line(int line_num) const;

    /// Returns StringRanges for a range of lines [start, end).
    std::vector<StringRange> get_lines(int start, int end) const;

    /// Returns the total number of lines.
    int line_count() const;

    /// Returns StringRanges for lines around a given line with context.
    std::vector<StringRange> get_context_lines(int line_num,
                                                int context_before,
                                                int context_after) const;

  private:
    StringPool* pool_;
    uint32_t file_id_;
    std::vector<StringRange> line_ranges_;
};

}  // namespace lci
