#include <lci/semantic/name_splitter.h>

#include <algorithm>
#include <cctype>

namespace lci {

NameSplitter::NameSplitter()
    : max_cache_size_(kDefaultCacheSize) {}

NameSplitter::NameSplitter(int cache_size)
    : max_cache_size_(cache_size > 0 ? cache_size : kDefaultCacheSize) {}

uint8_t NameSplitter::detect_separators(std::string_view name) {
    if (name.empty()) return kNone;

    uint8_t seps = kNone;
    for (size_t i = 0; i < name.size(); ++i) {
        auto ch = static_cast<unsigned char>(name[i]);

        switch (ch) {
            case '_': seps |= kUnderscore; break;
            case '-': seps |= kHyphen; break;
            case '.': seps |= kDot; break;
            case '/': seps |= kSlash; break;
            default: break;
        }

        if (i > 0) {
            auto prev = static_cast<unsigned char>(name[i - 1]);

            // lowercase to uppercase (camelCase).
            if (std::islower(prev) && std::isupper(ch)) {
                seps |= kCamelCase;
            }

            // uppercase followed by lowercase with preceding uppercase (PascalCase/acronym).
            if (std::isupper(prev) && std::islower(ch) && i > 1) {
                auto prev_prev = static_cast<unsigned char>(name[i - 2]);
                if (std::isupper(prev_prev)) {
                    seps |= kPascalCase;
                }
            }

            // letter-digit or digit-letter transitions.
            if ((std::isalpha(prev) && std::isdigit(ch)) ||
                (std::isdigit(prev) && std::isalpha(ch))) {
                seps |= kDigits;
            }
        }
    }

    return seps;
}

std::vector<std::string> NameSplitter::split_impl(std::string_view name, uint8_t seps) {
    std::vector<std::string> words;
    words.reserve(8);
    std::string word_buf;
    word_buf.reserve(64);

    for (size_t i = 0; i < name.size(); ++i) {
        auto ch = static_cast<unsigned char>(name[i]);

        // Explicit separator characters.
        if (ch == '_' || ch == '-' || ch == '.' || ch == '/') {
            if (!word_buf.empty()) {
                std::transform(word_buf.begin(), word_buf.end(),
                               word_buf.begin(), [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });
                words.push_back(std::move(word_buf));
                word_buf.clear();
            }
            continue;
        }

        // Case transitions for camelCase/PascalCase.
        if (i > 0 && (seps & (kCamelCase | kPascalCase))) {
            auto prev = static_cast<unsigned char>(name[i - 1]);

            // lowercase -> uppercase (camelCase boundary).
            if (std::islower(prev) && std::isupper(ch)) {
                if (!word_buf.empty()) {
                    std::transform(word_buf.begin(), word_buf.end(),
                                   word_buf.begin(), [](unsigned char c) {
                                       return static_cast<char>(std::tolower(c));
                                   });
                    words.push_back(std::move(word_buf));
                    word_buf.clear();
                }
            }

            // Uppercase followed by lowercase (HTTPServer -> HTTP, Server).
            if (i > 1 && std::isupper(prev) && std::islower(ch)) {
                auto prev_prev = static_cast<unsigned char>(name[i - 2]);
                if (std::isupper(prev_prev) && !word_buf.empty()) {
                    // Remove last char from buffer (it starts the new word).
                    char last_char = word_buf.back();
                    word_buf.pop_back();

                    if (!word_buf.empty()) {
                        std::transform(word_buf.begin(), word_buf.end(),
                                       word_buf.begin(), [](unsigned char c) {
                                           return static_cast<char>(std::tolower(c));
                                       });
                        words.push_back(std::move(word_buf));
                    }

                    word_buf.clear();
                    word_buf.push_back(last_char);
                }
            }
        }

        // Digit transitions.
        if (i > 0 && (seps & kDigits)) {
            auto prev = static_cast<unsigned char>(name[i - 1]);
            if ((std::isalpha(prev) && std::isdigit(ch)) ||
                (std::isdigit(prev) && std::isalpha(ch))) {
                if (!word_buf.empty()) {
                    std::transform(word_buf.begin(), word_buf.end(),
                                   word_buf.begin(), [](unsigned char c) {
                                       return static_cast<char>(std::tolower(c));
                                   });
                    words.push_back(std::move(word_buf));
                    word_buf.clear();
                }
            }
        }

        word_buf.push_back(static_cast<char>(ch));
    }

    // Flush final word.
    if (!word_buf.empty()) {
        std::transform(word_buf.begin(), word_buf.end(),
                       word_buf.begin(), [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        words.push_back(std::move(word_buf));
    }

    return words;
}

std::vector<std::string> NameSplitter::split(std::string_view name) const {
    if (name.empty()) return {};

    std::string key(name);

    // Check cache.
    {
        std::lock_guard lock(mu_);
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
    }

    // Detect separators.
    uint8_t seps = detect_separators(name);
    if (seps == kNone) {
        // No separators: return as-is (lowercase).
        std::string lower(name);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        std::vector<std::string> result{std::move(lower)};

        // Cache (no LRU eviction needed for single-word entries,
        // but respect max size).
        std::lock_guard lock(mu_);
        if (static_cast<int>(cache_.size()) < max_cache_size_) {
            cache_[key] = result;
            cache_keys_.push_back(key);
        }
        return result;
    }

    auto words = split_impl(name, seps);

    // Cache with LRU eviction.
    {
        std::lock_guard lock(mu_);
        if (static_cast<int>(cache_.size()) >= max_cache_size_ &&
            !cache_keys_.empty()) {
            cache_.erase(cache_keys_.front());
            cache_keys_.erase(cache_keys_.begin());
        }
        cache_[key] = words;
        cache_keys_.push_back(key);
    }

    return words;
}

std::unordered_set<std::string> NameSplitter::split_to_set(std::string_view name) const {
    auto words = split(name);
    std::unordered_set<std::string> result;
    result.reserve(words.size());
    for (auto& w : words) {
        if (!w.empty()) result.insert(std::move(w));
    }
    return result;
}

}  // namespace lci
