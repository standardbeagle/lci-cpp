#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lci {

/// Splits symbol names into constituent words.
/// Supports camelCase, PascalCase, snake_case, kebab-case, dot.notation,
/// path/notation, SCREAMING_SNAKE_CASE, and mixed formats.
///
/// Uses a two-pass approach: detect separator types, then split accordingly.
/// Results are cached with bounded LRU eviction for repeated lookups.
class NameSplitter {
  public:
    static constexpr int kDefaultCacheSize = 1000;

    NameSplitter();
    explicit NameSplitter(int cache_size);

    /// Splits a symbol name into lowercase constituent words.
    std::vector<std::string> split(std::string_view name) const;

    /// Splits a name and returns unique words as a set.
    std::unordered_set<std::string> split_to_set(std::string_view name) const;

  private:
    /// Bitflags for detected separator types.
    enum SeparatorType : uint8_t {
        kNone       = 0,
        kUnderscore = 1 << 0,
        kHyphen     = 1 << 1,
        kDot        = 1 << 2,
        kSlash      = 1 << 3,
        kCamelCase  = 1 << 4,
        kPascalCase = 1 << 5,
        kDigits     = 1 << 6,
    };

    /// First pass: identify separator types present in the name.
    static uint8_t detect_separators(std::string_view name);

    /// Second pass: split based on detected separators.
    static std::vector<std::string> split_impl(std::string_view name, uint8_t seps);

    /// Cache with bounded LRU eviction.
    mutable std::mutex mu_;
    mutable std::unordered_map<std::string, std::vector<std::string>> cache_;
    mutable std::vector<std::string> cache_keys_;
    int max_cache_size_;
};

}  // namespace lci
