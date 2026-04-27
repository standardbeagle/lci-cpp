#pragma once

#include <lci/parser/parser.h>

#include <vector>

struct TSParser;

namespace lci::parser {

/// Thread-local parser pool that recycles TSParser instances per language.
///
/// Each thread gets its own pool (via thread_local), eliminating contention.
/// Parsers are lazily created on first use and recycled via acquire/release.
/// All parsers are destroyed when the thread exits.
///
/// Mirrors the Go parserPools design in internal/parser/parser.go, but uses
/// thread_local instead of sync.Pool for zero-contention guarantees.
class ParserPool {
  public:
    ParserPool() = default;
    ~ParserPool() = default;

    ParserPool(const ParserPool&) = delete;
    ParserPool& operator=(const ParserPool&) = delete;
    ParserPool(ParserPool&&) = delete;
    ParserPool& operator=(ParserPool&&) = delete;

    /// Acquires a parser for the given language.
    /// Returns a recycled parser if available, otherwise creates a new one.
    /// Returns nullptr only if grammar loading fails.
    TSParser* acquire(Language lang);

    /// Returns a parser to the pool for the given language.
    /// The caller must not use the parser after releasing it.
    void release(Language lang, TSParser* parser);

    /// Returns the number of idle parsers for a language.
    std::size_t idle_count(Language lang) const;

  private:
    /// Per-language storage of idle parsers with RAII cleanup.
    struct LangSlot {
        std::vector<UniqueParser> idle;
    };

    LangSlot slots_[kLanguageCount]{};
};

/// Returns the thread-local parser pool for the calling thread.
ParserPool& thread_pool();

/// RAII guard that acquires a parser on construction and releases on destruction.
/// Prevents forgetting to return parsers to the pool.
class PooledParser {
  public:
    /// Acquires a parser for `lang` from the thread-local pool.
    explicit PooledParser(Language lang);
    ~PooledParser();

    PooledParser(const PooledParser&) = delete;
    PooledParser& operator=(const PooledParser&) = delete;
    PooledParser(PooledParser&& other) noexcept;
    PooledParser& operator=(PooledParser&& other) noexcept;

    /// Returns the underlying TSParser pointer (may be nullptr).
    TSParser* get() const { return parser_; }

    /// Returns true if a valid parser was acquired.
    explicit operator bool() const { return parser_ != nullptr; }

  private:
    Language lang_{};
    TSParser* parser_{};
};

}  // namespace lci::parser
