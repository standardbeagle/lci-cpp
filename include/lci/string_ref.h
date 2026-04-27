#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <lci/types.h>

namespace lci {

/// FNV-1a 64-bit offset basis.
inline constexpr uint64_t kFnvOffset64 = 14695981039346656037ULL;

/// FNV-1a 64-bit prime.
inline constexpr uint64_t kFnvPrime64 = 1099511628211ULL;

/// Computes FNV-1a hash of a string_view without allocations.
inline uint64_t hash_fnv1a(std::string_view s) {
    uint64_t h = kFnvOffset64;
    for (char c : s) {
        h ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        h *= kFnvPrime64;
    }
    return h;
}

/// Incorporates an int64 value into an existing FNV-1a hash.
inline uint64_t hash_fnv1a_combine(uint64_t h, int64_t val) {
    h ^= static_cast<uint64_t>(val);
    h *= kFnvPrime64;
    return h;
}

/// Lightweight, immutable reference to a substring within a file.
/// Provides zero-copy access via string_view when the backing data is available.
struct StringRef {
    FileID file_id{};
    uint32_t offset{};
    uint32_t length{};
    uint64_t hash{};

    /// Returns true if this is an empty/invalid reference.
    bool is_empty() const { return length == 0; }

    /// Extracts the referenced string as a view into the given content buffer.
    /// Returns empty view if bounds are invalid.
    std::string_view resolve(std::string_view content) const {
        if (offset + length > content.size()) return {};
        return content.substr(offset, length);
    }

    /// Extracts the referenced string (allocates).
    std::string to_string(std::string_view content) const {
        return std::string(resolve(content));
    }

    /// Fast equality check using hash first, then length and location.
    bool equal(const StringRef& other) const {
        if (hash != other.hash) return false;
        if (length != other.length) return false;
        if (file_id == other.file_id && offset == other.offset) return true;
        return false;
    }

    /// Fast comparison for sorting by hash, then file, then offset.
    int compare(const StringRef& other) const {
        if (hash < other.hash) return -1;
        if (hash > other.hash) return 1;
        if (file_id < other.file_id) return -1;
        if (file_id > other.file_id) return 1;
        if (offset < other.offset) return -1;
        if (offset > other.offset) return 1;
        return 0;
    }

    /// Checks if the given byte offset falls within this reference.
    bool contains(uint32_t off) const {
        return off >= offset && off < offset + length;
    }

    /// Checks if two references in the same file overlap.
    bool overlaps(const StringRef& other) const {
        if (file_id != other.file_id) return false;
        return offset < other.offset + other.length &&
               other.offset < offset + length;
    }

    /// Creates a sub-reference without accessing memory.
    /// Hash is zeroed and must be recomputed if needed.
    StringRef substring(uint32_t off, uint32_t len) const {
        if (off >= length) return {};
        if (off + len > length) len = length - off;
        return {file_id, offset + off, len, 0};
    }
};

/// Creates a StringRef from content bytes, computing the hash.
inline StringRef make_string_ref(FileID file_id, std::string_view content,
                                 uint32_t start, uint32_t len) {
    if (start + len > content.size()) return {};
    uint64_t h = hash_fnv1a(content.substr(start, len));
    return {file_id, start, len, h};
}

}  // namespace lci
