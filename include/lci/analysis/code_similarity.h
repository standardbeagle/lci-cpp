#pragma once

#include <string>
#include <string_view>

#include <absl/container/flat_hash_set.h>

namespace lci {

/// Strips comments and blank lines and trims per-line whitespace for
/// content comparison: two functions differing only in formatting or
/// comments normalize to the same string (exact-clone identity).
std::string normalize_code_content(std::string_view content);

/// Tokenizes a code block into the distinct-token set that
/// code_structural_similarity compares. Callers comparing one block
/// against many should tokenize each block exactly once.
absl::flat_hash_set<std::string> code_token_set(std::string_view content);

/// Jaccard similarity between two pre-tokenized code token sets.
double token_set_similarity(const absl::flat_hash_set<std::string>& a,
                            const absl::flat_hash_set<std::string>& b);

/// Computes Jaccard token-similarity between two code blocks.
double code_structural_similarity(std::string_view a, std::string_view b);

}  // namespace lci
