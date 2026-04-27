#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace lci {

/// Pre-computes line offsets and provides O(1) random access to lines.
/// Handles CRLF normalization: lines returned via line() strip trailing \r.
///
/// Usage:
///   LineScanner scanner(content);
///   for (int i = 0; i < scanner.line_count(); ++i) {
///       std::string_view line = scanner.line(i);  // O(1), zero-copy
///   }
class LineScanner {
  public:
    /// Constructs a scanner over the given content.
    /// Pre-computes all line offsets in a single pass.
    explicit LineScanner(std::string_view content);

    /// Returns the number of lines.
    int line_count() const;

    /// Returns line content at the given 0-based index. O(1).
    /// Returns empty view for out-of-bounds indices.
    /// Strips trailing \r for CRLF normalization.
    std::string_view line(int index) const;

    /// Returns the byte offset of a line start (0-based index). O(1).
    /// Returns 0 for out-of-bounds indices.
    uint32_t line_offset(int index) const;

    /// Returns the 1-based line number for a byte offset.
    /// Uses binary search for O(log n) performance.
    int line_at_offset(uint32_t byte_offset) const;

    /// Returns the raw pre-computed offsets vector.
    const std::vector<uint32_t>& offsets() const;

    /// Returns the underlying content.
    std::string_view content() const;

  private:
    std::string_view content_;
    std::vector<uint32_t> offsets_;
};

/// Counts lines in content without building an offset table.
/// Empty content returns 0. Matches Go CountLines semantics.
int count_lines(std::string_view content);

}  // namespace lci
