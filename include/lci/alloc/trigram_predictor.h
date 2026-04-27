#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace lci {

/// Expected usage statistics for a single trigram pattern.
struct TrigramStats {
    std::string trigram;
    double avg_per_file{};
    int max_per_file{};
    double confidence{};
    std::string category;
};

/// Predicts trigram occurrence counts for pre-allocation sizing.
///
/// Uses language-specific tables of common trigrams and a heuristic fallback
/// for unknown patterns. Supports online learning via update_stats().
class TrigramPredictor {
  public:
    /// Constructs a predictor for the specified language.
    /// Falls back to generic patterns for unknown languages.
    explicit TrigramPredictor(std::string_view language) {
        populate_patterns(language);
    }

    /// Estimates the appropriate pre-allocation capacity for a trigram
    /// given the file content for context.
    int predict_capacity(std::string_view trigram_str,
                         std::string_view file_content) const {
        auto it = patterns_.find(trigram_str);
        if (it == patterns_.end()) {
            return heuristic_estimate(trigram_str, file_content);
        }

        const auto& stats = it->second;
        int base_capacity = static_cast<int>(stats.avg_per_file);
        double content_multiplier = analyze_file_content(
            file_content, stats.category);
        int adjusted = static_cast<int>(
            static_cast<double>(base_capacity) * content_multiplier
            * stats.confidence);

        if (adjusted < 4) adjusted = 4;
        if (adjusted > stats.max_per_file * 2) {
            adjusted = stats.max_per_file * 2;
        }
        return adjusted;
    }

    /// Returns the most common trigrams, sorted by average frequency.
    std::vector<TrigramStats> get_common_trigrams(int limit) const {
        if (limit <= 0) limit = 20;
        std::vector<TrigramStats> result;
        result.reserve(patterns_.size());
        for (const auto& [key, stats] : patterns_) {
            result.push_back(stats);
        }
        std::sort(result.begin(), result.end(),
                  [](const TrigramStats& a, const TrigramStats& b) {
                      return a.avg_per_file > b.avg_per_file;
                  });
        if (static_cast<int>(result.size()) > limit) {
            result.resize(static_cast<size_t>(limit));
        }
        return result;
    }

    /// Updates statistics for a trigram based on actual observed count.
    void update_stats(std::string_view trigram_str, int actual_count) {
        auto it = patterns_.find(trigram_str);
        if (it == patterns_.end()) {
            TrigramStats s;
            s.trigram = std::string(trigram_str);
            s.avg_per_file = static_cast<double>(actual_count);
            s.max_per_file = actual_count;
            s.confidence = 0.3;
            s.category = "learned";
            patterns_[std::string(trigram_str)] = s;
            return;
        }

        constexpr double kAlpha = 0.1;
        auto& stats = it->second;
        stats.avg_per_file = (1.0 - kAlpha) * stats.avg_per_file
                             + kAlpha * static_cast<double>(actual_count);
        if (actual_count > stats.max_per_file) {
            stats.max_per_file = actual_count;
        }
        if (stats.confidence < 0.9) {
            stats.confidence += 0.05;
        }
    }

  private:
    absl::flat_hash_map<std::string, TrigramStats> patterns_;

    void populate_patterns(std::string_view language) {
        if (language == "go") {
            add("fun", 15.2, 89, 0.95, "keyword");
            add("unc", 14.8, 76, 0.94, "keyword");
            add("err", 12.3, 45, 0.92, "keyword");
            add("ret", 11.7, 52, 0.90, "keyword");
            add("var", 8.4, 31, 0.88, "keyword");
            add("tio", 9.1, 38, 0.85, "pattern");
            add("ion", 8.9, 42, 0.87, "pattern");
            add("tur", 7.2, 28, 0.82, "pattern");
            add("etu", 6.8, 25, 0.80, "pattern");
            add("pac", 4.2, 12, 0.70, "pattern");
            add("imp", 3.9, 10, 0.68, "pattern");
        } else if (language == "javascript") {
            add("fun", 18.7, 95, 0.93, "keyword");
            add("con", 14.2, 67, 0.90, "keyword");
            add("var", 12.8, 54, 0.88, "keyword");
            add("let", 11.3, 48, 0.85, "keyword");
            add("ret", 10.9, 44, 0.84, "keyword");
            add("ion", 9.8, 41, 0.82, "pattern");
            add("tio", 8.7, 35, 0.80, "pattern");
            add("ect", 7.1, 28, 0.78, "pattern");
        } else if (language == "python") {
            add("def", 16.3, 78, 0.91, "keyword");
            add("elf", 12.7, 56, 0.88, "keyword");
            add("ret", 10.8, 42, 0.83, "keyword");
            add("cla", 9.4, 38, 0.80, "keyword");
        } else if (language == "typescript") {
            add("fun", 17.1, 82, 0.90, "keyword");
            add("con", 13.8, 61, 0.87, "keyword");
            add("typ", 11.4, 48, 0.84, "keyword");
            add("int", 9.7, 39, 0.81, "keyword");
        } else {
            // Generic fallback.
            add("fun", 5.0, 20, 0.5, "keyword");
            add("err", 4.0, 15, 0.5, "keyword");
            add("ion", 3.5, 12, 0.5, "pattern");
        }
    }

    void add(const char* trigram, double avg, int max_count,
             double confidence, const char* category) {
        TrigramStats s;
        s.trigram = trigram;
        s.avg_per_file = avg;
        s.max_per_file = max_count;
        s.confidence = confidence;
        s.category = category;
        patterns_[s.trigram] = s;
    }

    static int heuristic_estimate(std::string_view trigram_str,
                                  std::string_view file_content) {
        int score = 0;
        if (trigram_str.size() == 3) score += 2;
        if (is_common_pattern(trigram_str)) score += 3;
        if (has_alpha(trigram_str)) score += 2;

        auto file_size = file_content.size();
        if (file_size > 10000) {
            score += 2;
        } else if (file_size > 5000) {
            score += 1;
        }

        if (score <= 1) return 4;
        if (score <= 3) return 8;
        if (score <= 5) return 16;
        if (score <= 7) return 32;
        return 64;
    }

    static double analyze_file_content(std::string_view content,
                                       std::string_view category) {
        double multiplier = 1.0;

        if (category == "keyword") {
            if (content.find("func") != std::string_view::npos
                || content.find("function") != std::string_view::npos) {
                multiplier += 0.3;
            }
            if (content.find("err") != std::string_view::npos
                || content.find("error") != std::string_view::npos) {
                multiplier += 0.4;
            }
        } else if (category == "operator") {
            if (count_occurrences(content, "==") > 5) multiplier += 0.2;
            if (count_occurrences(content, ":=") > 3) multiplier += 0.3;
        } else if (category == "pattern") {
            auto c = count_occurrences(content, "tion");
            if (c > 20) {
                multiplier += 0.5;
            } else if (c > 10) {
                multiplier += 0.2;
            }
        }

        auto line_count = count_occurrences(content, "\n");
        if (line_count > 500) {
            multiplier += 0.2;
        } else if (line_count > 200) {
            multiplier += 0.1;
        }

        if (multiplier > 2.0) multiplier = 2.0;
        if (multiplier < 0.5) multiplier = 0.5;
        return multiplier;
    }

    static int count_occurrences(std::string_view haystack,
                                 std::string_view needle) {
        int count = 0;
        size_t pos = 0;
        while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
            ++count;
            pos += needle.size();
        }
        return count;
    }

    static bool is_common_pattern(std::string_view trigram) {
        static constexpr std::string_view kPatterns[] = {
            "fun", "unc", "err", "ret", "var", "let", "con", "def", "cla",
            "tio", "ion", "tur", "etu", "ect", "pac", "imp", "ass", "typ",
        };
        for (auto p : kPatterns) {
            if (trigram.find(p) != std::string_view::npos) return true;
        }
        return false;
    }

    static bool has_alpha(std::string_view s) {
        for (char c : s) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9')) {
                return true;
            }
        }
        return false;
    }
};

}  // namespace lci
