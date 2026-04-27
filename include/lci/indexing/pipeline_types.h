#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <lci/core/trigram.h>
#include <lci/error.h>
#include <lci/reference.h>
#include <lci/scope.h>
#include <lci/symbol.h>
#include <lci/types.h>

namespace lci {

/// A file discovered by the scanner, ready for processing.
struct FileTask {
    std::string path;
    std::string language;
    int64_t size{};
    int priority{};
};

/// Result of processing a single file through the pipeline.
struct ProcessedFile {
    std::string path;
    FileID file_id{};
    std::vector<Symbol> symbols;
    std::vector<EnhancedSymbol> enhanced_symbols;
    std::vector<Reference> references;
    std::vector<ScopeInfo> scopes;
    BucketedTrigramResult bucketed_trigrams;
    std::vector<int> line_offsets;
    std::string language;
    std::string stage;
    std::chrono::nanoseconds duration{};
    Error error{};
    bool has_error{};
};

/// Pipeline buffer size constants.
namespace pipeline_constants {
inline constexpr int kTaskChannelBaseMultiplier = 8;
inline constexpr int kResultChannelBaseMultiplier = 16;
inline constexpr int kMaxTaskChannelBuffer = 1000;
inline constexpr int kMaxResultChannelBuffer = 2000;
inline constexpr auto kTaskChannelTimeout = std::chrono::seconds(5);
inline constexpr int kMaxBackPressureRetries = 10;
}  // namespace pipeline_constants

/// Calculates optimal channel buffer sizes based on CPU count and file count.
std::pair<int, int> calculate_optimal_channel_buffers(int file_count);

}  // namespace lci
