#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <absl/container/flat_hash_map.h>

namespace lci {

/// Detects binary files that should not be indexed.
/// Uses extension-based fast path and magic-number content analysis.
class BinaryDetector {
  public:
    BinaryDetector();

    /// Returns true if the file extension indicates binary content.
    bool is_binary_by_extension(std::string_view path) const;

    /// Returns true if the first bytes contain binary signatures.
    /// Checks magic numbers and non-printable byte ratios.
    bool is_binary_by_magic_number(std::string_view content) const;

    /// Combines extension and magic number checks.
    bool is_binary(std::string_view path, std::string_view content) const;

  private:
    absl::flat_hash_map<std::string, bool> binary_extensions_;
};

}  // namespace lci
