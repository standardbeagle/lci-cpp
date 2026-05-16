#include <lci/regex_analyzer/engine.h>

#include <algorithm>
#include <cctype>

#include <re2/re2.h>

namespace lci {

namespace {

// Build RE2 options matching the engine's prior std::regex defaults:
// case-insensitive optional, log-silent (failures reported via nullptr).
//
// Note on multiline anchors: RE2's `set_one_line(false)` only applies in
// POSIX syntax mode. In default Perl mode (what we use here, matching the
// previous std::regex::ECMAScript surface), `^`/`$` are opt-in per-line via
// the `(?m)` inline flag. We attach `(?m)` in compile() so the engine's
// public behavior matches the prior std::regex::multiline default.
RE2::Options make_options(bool case_insensitive) {
    RE2::Options opts(RE2::Quiet);
    opts.set_case_sensitive(!case_insensitive);
    opts.set_log_errors(false);
    return opts;
}

// Build a small immutable RE2 used for internal classification/extraction
// (case-sensitive, no multi-line semantics needed for the lci-cpp callers).
std::unique_ptr<RE2> make_internal_re2(const char* pat) {
    RE2::Options opts(RE2::Quiet);
    opts.set_log_errors(false);
    return std::make_unique<RE2>(pat, opts);
}

}  // namespace

// -- RegexClassifier ----------------------------------------------------------

RegexClassifier::RegexClassifier() {
    // Pre-compiled patterns indicating complexity.
    // These detect constructs that prevent trigram optimization.
    //
    // Karpathy: compiled ONCE at ctor, stored in a vector of unique_ptrs.
    // Each is_simple() call merely runs RE2::PartialMatch — no recompile.
    const char* patterns[] = {
        R"(\(\?[=!])",           // lookaheads: (?= or (?!
        R"(\(\?<)",              // lookbehind or named group
        R"(\\\d+)",              // backreferences: \1, \2
        R"(\(\?\()",             // conditional groups
        R"(\(\?>)",              // atomic groups
        R"(\(\?[imsx-]+:)",      // inline modifiers: (?i:
        R"([*+?]\+)",            // possessive quantifiers
        R"(\(\?R\)|\(\?0\))",    // recursive patterns
        R"(\(\?&\w+\))",         // subroutine calls
    };
    complex_patterns_.reserve(std::size(patterns));
    for (const auto* p : patterns) {
        complex_patterns_.emplace_back(make_internal_re2(p));
    }
}

bool RegexClassifier::is_simple(std::string_view pattern) const {
    if (pattern.empty()) return false;

    re2::StringPiece sp(pattern.data(), pattern.size());
    for (const auto& re : complex_patterns_) {
        if (RE2::PartialMatch(sp, *re)) return false;
    }

    return is_structurally_simple(pattern);
}

bool RegexClassifier::is_structurally_simple(std::string_view pattern) const {
    if (!is_balanced(pattern)) return false;
    if (calculate_nesting_depth(pattern) > 5) return false;
    if (has_long_alternations(pattern)) return false;
    return true;
}

bool RegexClassifier::is_balanced(std::string_view pattern) const {
    int parens = 0;
    int braces = 0;
    bool in_char_class = false;
    bool escaped = false;

    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (escaped) { escaped = false; continue; }

        switch (c) {
            case '\\': escaped = true; break;
            case '[':
                if (!in_char_class) in_char_class = true;
                break;
            case ']':
                if (in_char_class && i > 0 && pattern[i - 1] != '\\')
                    in_char_class = false;
                break;
            case '(':
                if (!in_char_class) ++parens;
                break;
            case ')':
                if (!in_char_class) { --parens; if (parens < 0) return false; }
                break;
            case '{':
                if (!in_char_class) ++braces;
                break;
            case '}':
                if (!in_char_class) { --braces; if (braces < 0) return false; }
                break;
            default: break;
        }
    }

    return parens == 0 && braces == 0;
}

int RegexClassifier::calculate_nesting_depth(std::string_view pattern) const {
    int max_depth = 0;
    int current_depth = 0;
    bool in_char_class = false;
    bool escaped = false;

    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (escaped) { escaped = false; continue; }

        switch (c) {
            case '\\': escaped = true; break;
            case '[':
                if (!in_char_class) in_char_class = true;
                break;
            case ']':
                if (in_char_class && i > 0 && pattern[i - 1] != '\\')
                    in_char_class = false;
                break;
            case '(':
                if (!in_char_class) {
                    ++current_depth;
                    if (current_depth > max_depth) max_depth = current_depth;
                }
                break;
            case ')':
                if (!in_char_class && current_depth > 0) --current_depth;
                break;
            default: break;
        }
    }

    return max_depth;
}

bool RegexClassifier::has_long_alternations(std::string_view pattern) const {
    int alternation_count = 0;
    for (char c : pattern) {
        if (c == '|') ++alternation_count;
    }
    if (alternation_count > 20) return true;

    // Check for very long alternation parts.
    size_t part_start = 0;
    for (size_t i = 0; i <= pattern.size(); ++i) {
        if (i == pattern.size() || pattern[i] == '|') {
            if (i - part_start > 1000) return true;
            part_start = i + 1;
        }
    }

    return false;
}

// -- LiteralExtractor ---------------------------------------------------------

LiteralExtractor::LiteralExtractor()
    // RE2::FindAndConsume requires a capturing group per output argument.
    // literal_pattern_ wraps the run in a capture group so the whole-run text
    // lands in our std::string output. alternation_pattern_ already has one.
    : literal_pattern_(make_internal_re2(R"(([a-zA-Z0-9_]{3,}))")),
      alternation_pattern_(
          make_internal_re2(R"(\(([a-zA-Z0-9_]+(?:\|[a-zA-Z0-9_]+)*)\))")) {}

std::vector<std::string> LiteralExtractor::extract_literals(
    std::string_view pattern) const {

    std::vector<std::string> literals;
    absl::flat_hash_map<std::string, bool> seen;

    // Extract from alternations first (highest priority).
    auto alt_literals = extract_from_alternations(pattern);
    for (auto& lit : alt_literals) {
        if (lit.size() >= 3 && has_alphanumeric(lit) && !seen[lit]) {
            seen[lit] = true;
            literals.push_back(std::move(lit));
        }
    }

    // Extract general literals.
    auto gen_literals = extract_general_literals(pattern);
    for (auto& lit : gen_literals) {
        if (lit.size() >= 3 && has_alphanumeric(lit) && !seen[lit]) {
            seen[lit] = true;
            literals.push_back(std::move(lit));
        }
    }

    return literals;
}

bool LiteralExtractor::has_alphanumeric(std::string_view s) const {
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) return true;
    }
    return false;
}

std::vector<std::string> LiteralExtractor::extract_from_alternations(
    std::string_view pattern) const {

    std::vector<std::string> literals;
    re2::StringPiece input(pattern.data(), pattern.size());
    std::string content;

    // RE2::FindAndConsume advances `input` past each match, giving us iterate-
    // all-matches semantics without per-iteration std::string copies of the
    // input (StringPiece is zero-copy over the caller's buffer).
    while (RE2::FindAndConsume(&input, *alternation_pattern_, &content)) {
        // Split on '|' and collect alternatives >= 3 chars.
        size_t start = 0;
        for (size_t i = 0; i <= content.size(); ++i) {
            if (i == content.size() || content[i] == '|') {
                if (i - start >= 3) {
                    literals.push_back(content.substr(start, i - start));
                }
                start = i + 1;
            }
        }
    }

    return literals;
}

std::vector<std::string> LiteralExtractor::extract_general_literals(
    std::string_view pattern) const {

    std::vector<std::string> literals;
    re2::StringPiece input(pattern.data(), pattern.size());
    std::string match;

    while (RE2::FindAndConsume(&input, *literal_pattern_, &match)) {
        if (match.size() >= 3) {
            literals.push_back(std::move(match));
        }
    }

    return literals;
}

// -- RegexCache ---------------------------------------------------------------

RegexCache::RegexCache(int max_simple_size, int max_complex_size)
    : max_simple_size_(max_simple_size),
      max_complex_size_(max_complex_size) {}

std::pair<SimpleRegexPattern*, RE2*> RegexCache::get_regex(
    std::string_view pattern, bool case_insensitive) {

    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.total_requests;

    if (static_cast<int>(pattern.size()) > kMaxPatternLength) {
        return {nullptr, nullptr};
    }

    auto key = build_cache_key(pattern, case_insensitive);

    // Check simple cache.
    if (auto it = simple_cache_.find(key); it != simple_cache_.end()) {
        it->second.access_count++;
        // Move to front of LRU.
        simple_lru_.remove(key);
        simple_lru_.push_front(key);
        ++stats_.simple_hits;
        return {&it->second, nullptr};
    }

    // Check complex cache.
    if (auto it = complex_cache_.find(key); it != complex_cache_.end()) {
        complex_lru_.remove(key);
        complex_lru_.push_front(key);
        ++stats_.complex_hits;
        return {nullptr, it->second.get()};
    }

    ++stats_.simple_misses;
    ++stats_.complex_misses;
    return {nullptr, nullptr};
}

void RegexCache::cache_simple(SimpleRegexPattern pattern,
                              bool case_insensitive) {
    std::lock_guard<std::mutex> lock(mu_);

    auto key = build_cache_key(pattern.pattern, case_insensitive);
    if (simple_cache_.contains(key)) return;

    if (static_cast<int>(simple_cache_.size()) >= max_simple_size_) {
        evict_simple();
    }

    pattern.cache_key = key;
    pattern.access_count = 1;
    simple_lru_.push_front(key);
    simple_cache_[key] = std::move(pattern);
}

void RegexCache::cache_complex(std::string_view pattern,
                               std::shared_ptr<RE2> compiled,
                               bool case_insensitive) {
    std::lock_guard<std::mutex> lock(mu_);

    auto key = build_cache_key(pattern, case_insensitive);
    if (complex_cache_.contains(key)) return;

    if (static_cast<int>(complex_cache_.size()) >= max_complex_size_) {
        evict_complex();
    }

    complex_cache_[key] = std::move(compiled);
    complex_lru_.push_front(key);
}

std::string RegexCache::build_cache_key(std::string_view pattern,
                                        bool case_insensitive) const {
    std::string key;
    if (case_insensitive) {
        key = "(?i)";
    }
    key.append(pattern);
    return key;
}

void RegexCache::evict_simple() {
    if (simple_lru_.empty()) return;
    auto& back = simple_lru_.back();
    simple_cache_.erase(back);
    simple_lru_.pop_back();
    ++stats_.simple_evictions;
}

void RegexCache::evict_complex() {
    if (complex_lru_.empty()) return;
    auto& back = complex_lru_.back();
    complex_cache_.erase(back);
    complex_lru_.pop_back();
    ++stats_.complex_evictions;
}

CacheStats RegexCache::get_stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
}

void RegexCache::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    simple_cache_.clear();
    complex_cache_.clear();
    simple_lru_.clear();
    complex_lru_.clear();
    stats_ = {};
}

std::pair<int, int> RegexCache::get_size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return {static_cast<int>(simple_cache_.size()),
            static_cast<int>(complex_cache_.size())};
}

double RegexCache::get_hit_ratio() const {
    std::lock_guard<std::mutex> lock(mu_);
    auto total = stats_.simple_hits + stats_.simple_misses;
    if (total == 0) return 0.0;
    return static_cast<double>(stats_.simple_hits) / static_cast<double>(total);
}

double RegexCache::get_complex_hit_ratio() const {
    std::lock_guard<std::mutex> lock(mu_);
    auto total = stats_.complex_hits + stats_.complex_misses;
    if (total == 0) return 0.0;
    return static_cast<double>(stats_.complex_hits) /
           static_cast<double>(total);
}

// -- HybridRegexEngine --------------------------------------------------------

HybridRegexEngine::HybridRegexEngine(int simple_cache_size,
                                     int complex_cache_size)
    : cache_(simple_cache_size, complex_cache_size) {}

std::vector<std::string> HybridRegexEngine::extract_literals(
    std::string_view pattern) const {
    return extractor_.extract_literals(pattern);
}

bool HybridRegexEngine::is_simple(std::string_view pattern) const {
    return classifier_.is_simple(pattern);
}

CacheStats HybridRegexEngine::get_cache_stats() const {
    return cache_.get_stats();
}

void HybridRegexEngine::clear_cache() {
    cache_.clear();
}

std::shared_ptr<RE2> HybridRegexEngine::compile(
    std::string_view pattern, bool case_insensitive) const {

    auto opts = make_options(case_insensitive);
    // Prefix `(?m)` so `^` and `$` behave per-line, matching the prior
    // std::regex::multiline default. The prefix is benign for patterns that
    // don't use those anchors.
    std::string with_multiline;
    with_multiline.reserve(pattern.size() + 4);
    with_multiline.append("(?m)");
    with_multiline.append(pattern);
    auto re = std::make_shared<RE2>(with_multiline, opts);
    if (!re->ok()) {
        return nullptr;
    }
    return re;
}

}  // namespace lci
