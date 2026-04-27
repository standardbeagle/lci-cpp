#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/analysis/codebase_intelligence_types.h>
#include <lci/symbol.h>

namespace lci {

/// Classifies symbol names into domain categories for codebase intelligence.
///
/// Distinct from the semantic VocabularyAnalyzer which handles name splitting
/// and scoring. This analyzer maps terms to domain patterns (Authentication,
/// Database, HTTP/API, etc.) with confidence scoring.
///
/// Ported from Go: codebase_intelligence_vocabulary.go
class CIVocabularyAnalyzer {
  public:
    CIVocabularyAnalyzer();

    /// Classifies a term into a domain with match strength (0.0-1.0).
    std::pair<std::string, double> classify_term_with_strength(
        std::string_view term) const;

    /// Classifies a term into a domain (simplified wrapper).
    std::string classify_term(std::string_view term) const;

    /// Calculates domain confidence from match parameters.
    static double calculate_domain_confidence(
        double match_strength, int term_count, int total_frequency,
        int total_terms);

    /// Extracts domain terms from file symbol data.
    std::vector<DomainTerm> extract_domain_terms_from_files(
        const std::vector<FileSymbolData>& files) const;

  private:
    absl::flat_hash_map<std::string, DomainPattern> domain_patterns_;
};

}  // namespace lci
