#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <lci/reference.h>
#include <lci/scope.h>
#include <lci/types.h>

namespace lci {

/// Forward declaration.
struct EnhancedSymbol;

/// ASCII bitmask for fast character presence checks (256 bits = 4 x uint64_t).
struct CharMask {
    std::array<uint64_t, 4> ascii_mask{};
    bool has_unicode{};
};

/// Information about a loop for performance analysis.
struct LoopData {
    std::string node_type;
    int start_line{};
    int end_line{};
    int depth{};
};

/// Information about an await expression for performance analysis.
struct AwaitData {
    int line{};
    std::string assigned_var;
    std::string call_target;
    std::vector<std::string> used_vars;
};

/// Information about a function call for performance analysis.
struct CallData {
    std::string target;
    int line{};
    bool in_loop{};
    int loop_depth{};
    int loop_line{};
};

/// Performance analysis data for a single function.
struct FunctionPerfData {
    std::string name;
    int start_line{};
    int end_line{};
    bool is_async{};
    std::string language;
    std::vector<LoopData> loops;
    std::vector<AwaitData> awaits;
    std::vector<CallData> calls;
};

/// Complete metadata for an indexed source file.
/// Matches Go's types.FileInfo struct (excluding content storage and index logic).
struct FileInfo {
    FileID id{};
    std::string path;
    uint64_t checksum{};
    uint64_t fast_hash{};
    std::array<uint8_t, 32> content_hash{};
    std::vector<BlockBoundary> blocks;
    std::vector<Import> imports;
    std::vector<Reference> references;
    std::vector<ScopeInfo> scope_hierarchy;
    CharMask char_mask;
    std::vector<int> line_offsets;
    std::vector<FunctionPerfData> perf_data;
};

}  // namespace lci
