#include <lci/parser/parser_pool.h>

#include <tree_sitter/api.h>

#include <utility>

namespace lci::parser {

TSParser* ParserPool::acquire(Language lang) {
    auto idx = static_cast<std::size_t>(lang);
    auto& slot = slots_[idx];

    if (!slot.idle.empty()) {
        // Recycle an existing parser.
        UniqueParser p = std::move(slot.idle.back());
        slot.idle.pop_back();
        return p.release();
    }

    // Create a new parser for this language.
    UniqueParser p = make_parser(lang);
    return p.release();
}

void ParserPool::release(Language lang, TSParser* parser) {
    if (!parser) return;

    auto idx = static_cast<std::size_t>(lang);
    // Reset parser state so it can be reused cleanly.
    ts_parser_reset(parser);
    slots_[idx].idle.emplace_back(UniqueParser(parser));
}

std::size_t ParserPool::idle_count(Language lang) const {
    auto idx = static_cast<std::size_t>(lang);
    return slots_[idx].idle.size();
}

ParserPool& thread_pool() {
    thread_local ParserPool pool;
    return pool;
}

// PooledParser implementation

PooledParser::PooledParser(Language lang)
    : lang_(lang)
    , parser_(thread_pool().acquire(lang)) {}

PooledParser::~PooledParser() {
    if (parser_) {
        thread_pool().release(lang_, parser_);
    }
}

PooledParser::PooledParser(PooledParser&& other) noexcept
    : lang_(other.lang_)
    , parser_(other.parser_) {
    other.parser_ = nullptr;
}

PooledParser& PooledParser::operator=(PooledParser&& other) noexcept {
    if (this != &other) {
        if (parser_) {
            thread_pool().release(lang_, parser_);
        }
        lang_ = other.lang_;
        parser_ = other.parser_;
        other.parser_ = nullptr;
    }
    return *this;
}

}  // namespace lci::parser
