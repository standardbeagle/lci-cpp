#pragma once

#include <cstdint>
#include <list>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace lci {

// -- Pattern classification ---------------------------------------------------

/// Classifies regex patterns as simple (trigram-optimizable) or complex.
class RegexClassifier {
  public:
    RegexClassifier();

    /// Returns true if the pattern is simple enough for trigram optimization.
    bool is_simple(std::string_view pattern) const;

  private:
    std::vector<std::regex> complex_patterns_;

    bool is_structurally_simple(std::string_view pattern) const;
    bool is_balanced(std::string_view pattern) const;
    int calculate_nesting_depth(std::string_view pattern) const;
    bool has_long_alternations(std::string_view pattern) const;
};

// -- Literal extraction -------------------------------------------------------

/// Extracts literal strings from regex patterns for trigram filtering.
class LiteralExtractor {
  public:
    LiteralExtractor();

    /// Extracts literal strings suitable for trigram filtering (>= 3 chars).
    std::vector<std::string> extract_literals(std::string_view pattern) const;

  private:
    std::regex literal_pattern_;
    std::regex alternation_pattern_;

    bool has_alphanumeric(std::string_view s) const;
    std::vector<std::string> extract_from_alternations(
        std::string_view pattern) const;
    std::vector<std::string> extract_general_literals(
        std::string_view pattern) const;
};

// -- Execution path -----------------------------------------------------------

/// Indicates which execution path was taken for regex search.
enum class ExecutionPath : uint8_t {
    SimpleTrigramFiltered = 0,
    SimpleNoTrigrams,
    ComplexDirect,
    ComplexFiltered,
    Error,
};

// -- Cached simple pattern ----------------------------------------------------

/// A parsed and cached simple regex pattern with extracted literals.
struct SimpleRegexPattern {
    std::string pattern;
    std::vector<std::string> literals;
    std::regex compiled;
    std::string cache_key;
    int64_t access_count{};
};

// -- Cache statistics ---------------------------------------------------------

/// Tracks cache performance statistics.
struct CacheStats {
    int64_t simple_hits{};
    int64_t simple_misses{};
    int64_t complex_hits{};
    int64_t complex_misses{};
    int64_t simple_evictions{};
    int64_t complex_evictions{};
    int64_t total_requests{};
};

// -- Regex cache --------------------------------------------------------------

/// LRU cache for compiled regex patterns (both simple and complex).
class RegexCache {
  public:
    RegexCache(int max_simple_size, int max_complex_size);

    /// Looks up a pattern in cache. Returns pointers (nullptr if miss).
    std::pair<SimpleRegexPattern*, std::regex*> get_regex(
        std::string_view pattern, bool case_insensitive);

    /// Caches a simple regex pattern.
    void cache_simple(SimpleRegexPattern pattern, bool case_insensitive);

    /// Caches a complex compiled regex.
    void cache_complex(std::string_view pattern,
                       std::regex compiled,
                       bool case_insensitive);

    /// Returns cache statistics.
    CacheStats get_stats() const;

    /// Clears all cached patterns.
    void clear();

    /// Returns current (simple, complex) cache sizes.
    std::pair<int, int> get_size() const;

    /// Returns simple cache hit ratio.
    double get_hit_ratio() const;

    /// Returns complex cache hit ratio.
    double get_complex_hit_ratio() const;

  private:
    absl::flat_hash_map<std::string, SimpleRegexPattern> simple_cache_;
    absl::flat_hash_map<std::string, std::regex> complex_cache_;
    std::list<std::string> simple_lru_;
    std::list<std::string> complex_lru_;

    mutable std::mutex mu_;
    int max_simple_size_;
    int max_complex_size_;
    static constexpr int kMaxPatternLength = 1000;
    CacheStats stats_;

    std::string build_cache_key(std::string_view pattern,
                                bool case_insensitive) const;
    void evict_simple();
    void evict_complex();
};

// -- Hybrid regex engine ------------------------------------------------------

/// Combines classifier, cache, and literal extraction for optimized
/// regex searching with trigram filtering.
class HybridRegexEngine {
  public:
    HybridRegexEngine(int simple_cache_size, int complex_cache_size);

    /// Extracts literal strings from a regex pattern for trigram filtering.
    std::vector<std::string> extract_literals(std::string_view pattern) const;

    /// Returns true if the pattern is simple (trigram-optimizable).
    bool is_simple(std::string_view pattern) const;

    /// Returns cache statistics.
    CacheStats get_cache_stats() const;

    /// Clears all cached patterns.
    void clear_cache();

    /// Compiles a regex pattern with multiline mode and optional
    /// case-insensitive flag. Returns nullptr on failure.
    std::unique_ptr<std::regex> compile(std::string_view pattern,
                                         bool case_insensitive) const;

  private:
    RegexClassifier classifier_;
    RegexCache cache_;
    LiteralExtractor extractor_;
};

}  // namespace lci
