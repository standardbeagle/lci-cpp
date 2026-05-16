#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <re2/re2.h>

namespace lci {

// -- Pattern classification ---------------------------------------------------

/// Classifies regex patterns as simple (trigram-optimizable) or complex.
///
/// Karpathy: complex-pattern detection runs on every regex compile request.
/// RE2 instances are constructed once at ctor and reused across calls — no
/// per-call allocation, no std::regex backtracking engine.
class RegexClassifier {
  public:
    RegexClassifier();

    // Non-copyable, non-movable because RE2 instances inside complex_patterns_
    // are not copyable; the classifier is held by value inside HybridRegexEngine
    // for its lifetime and never re-seated.
    RegexClassifier(const RegexClassifier&) = delete;
    RegexClassifier& operator=(const RegexClassifier&) = delete;

    /// Returns true if the pattern is simple enough for trigram optimization.
    bool is_simple(std::string_view pattern) const;

  private:
    // unique_ptr because RE2 itself is non-copyable, non-movable. Vector of
    // unique_ptrs gives us a stable, reusable set of compiled detectors.
    std::vector<std::unique_ptr<RE2>> complex_patterns_;

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

    LiteralExtractor(const LiteralExtractor&) = delete;
    LiteralExtractor& operator=(const LiteralExtractor&) = delete;

    /// Extracts literal strings suitable for trigram filtering (>= 3 chars).
    std::vector<std::string> extract_literals(std::string_view pattern) const;

  private:
    // Held by unique_ptr because RE2 is non-copyable/movable. Reused across
    // every extract_literals call — no per-call compilation.
    std::unique_ptr<RE2> literal_pattern_;
    std::unique_ptr<RE2> alternation_pattern_;

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
///
/// `compiled` is shared_ptr<RE2> so cache entries can be inspected via the
/// pointer returned by RegexCache::get_regex without transferring ownership.
/// RE2 is thread-safe for matching once compiled.
struct SimpleRegexPattern {
    std::string pattern;
    std::vector<std::string> literals;
    std::shared_ptr<RE2> compiled;
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
///
/// Holds shared_ptr<RE2> for complex entries so callers can keep the compiled
/// regex alive past a cache eviction. RE2's match methods are const-thread-safe.
class RegexCache {
  public:
    RegexCache(int max_simple_size, int max_complex_size);

    /// Looks up a pattern in cache. Returns pointers (nullptr if miss).
    /// The returned SimpleRegexPattern* points into the cache; the RE2 ptr is
    /// the raw underlying compiled regex (caller must not delete).
    std::pair<SimpleRegexPattern*, RE2*> get_regex(
        std::string_view pattern, bool case_insensitive);

    /// Caches a simple regex pattern.
    void cache_simple(SimpleRegexPattern pattern, bool case_insensitive);

    /// Caches a complex compiled regex. Takes ownership of the RE2 instance.
    void cache_complex(std::string_view pattern,
                       std::shared_ptr<RE2> compiled,
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
    absl::flat_hash_map<std::string, std::shared_ptr<RE2>> complex_cache_;
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
    ///
    /// RE2 multiline semantics: `^` and `$` already match line boundaries with
    /// RE2::Options::set_one_line(false) (the default for multi-line corpora).
    std::shared_ptr<RE2> compile(std::string_view pattern,
                                  bool case_insensitive) const;

  private:
    RegexClassifier classifier_;
    RegexCache cache_;
    LiteralExtractor extractor_;
};

}  // namespace lci
