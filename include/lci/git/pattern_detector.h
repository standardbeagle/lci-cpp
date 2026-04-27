#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <lci/git/types.h>

namespace lci {
namespace git {

/// Identifies conflict-prone code patterns in file content.
/// Ported from Go: internal/git/pattern_detector.go
class PatternDetector {
  public:
    PatternDetector();

    /// Analyzes file content for conflict-prone patterns.
    std::vector<AntiPattern> detect_patterns(std::string_view content,
                                             std::string_view file_path) const;

    /// Performs a fast scan looking only for the most impactful patterns.
    /// Returns true and fills `out` if a pattern was found.
    bool quick_scan(std::string_view content, std::string_view file_path,
                    AntiPattern& out) const;

    /// Returns detailed recommendations for a pattern type.
    static std::vector<std::string> get_recommendations(AntiPatternType type);

    /// Returns the line count of content.
    static int count_lines(std::string_view content);

    // Configurable thresholds
    int registration_calls_threshold{10};
    int enum_values_threshold{10};
    int god_object_lines_threshold{1500};
    int switch_cases_threshold{10};
    int config_fields_threshold{10};
    double barrel_export_ratio_threshold{0.5};

  private:
    bool detect_registration_function(std::string_view content,
                                      std::string_view file_path,
                                      AntiPattern& out) const;
    bool detect_enum_aggregation(std::string_view content,
                                 std::string_view file_path,
                                 AntiPattern& out) const;
    bool detect_god_object(std::string_view content,
                           std::string_view file_path,
                           AntiPattern& out) const;
    std::vector<AntiPattern> detect_switch_factories(std::string_view content,
                                                     std::string_view file_path) const;
    bool detect_barrel_file(std::string_view content,
                            std::string_view file_path,
                            AntiPattern& out) const;
    bool detect_config_aggregation(std::string_view content,
                                   std::string_view file_path,
                                   AntiPattern& out) const;
};

/// Calculates severity based on how much threshold is exceeded.
AntiPatternSeverity determine_anti_pattern_severity(int count, int threshold);

/// Counts case statements within a switch block starting at offset.
int count_cases_in_switch(std::string_view content, size_t start_idx);

/// Checks if a byte is alphanumeric or underscore.
inline bool is_alnum_or_underscore(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

}  // namespace git
}  // namespace lci
