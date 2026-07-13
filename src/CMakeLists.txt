add_library(lci_lib STATIC
    config/config.cpp
    error.cpp
    string_pool.cpp
    core/file_content_store.cpp
    core/file_loader.cpp
    core/file_service.cpp
    core/line_scanner.cpp
    core/trigram.cpp
    core/symbol_store.cpp
    core/trigram_bucketing.cpp
    core/reference_tracker.cpp
    core/import_resolver.cpp
    core/portable.cpp
    core/subprocess.cpp
    core/postings.cpp
    parser/parser.cpp
    parser/parser_pool.cpp
    parser/parse_methods.cpp
    parser/unified_extractor.cpp
    parser/unified_extractor_references.cpp
    parser/unified_extractor_side_effects.cpp
    parser/unified_extractor_types.cpp
    config/gitignore.cpp
    indexing/binary_detector.cpp
    indexing/pipeline_scanner.cpp
    indexing/pipeline_processor.cpp
    indexing/pipeline_integrator.cpp
    indexing/trigram_merger.cpp
    indexing/pipeline_progress.cpp
    indexing/pipeline.cpp
    indexing/master_index.cpp
    indexing/master_index_search.cpp
    indexing/deleted_file_tracker.cpp
    indexing/debounced_rebuilder.cpp
    indexing/watcher.cpp
    search/engine.cpp
    search/search_coordinator.cpp
    search/engine_context.cpp
    search/requirements_analyzer.cpp
    search/semantic_filter.cpp
    semantic/score_types.cpp
    semantic/name_splitter.cpp
    semantic/stemmer.cpp
    semantic/fuzzy_matcher.cpp
    semantic/semantic_scorer.cpp
    semantic/synonym_table.cpp
    semantic/vocabulary_analyzer.cpp
    analysis/call_graph.cpp
    analysis/side_effect_analyzer.cpp
    analysis/health_analyzer.cpp
    analysis/ci_vocabulary_analyzer.cpp
    analysis/coupling_analyzer.cpp
    analysis/feature_analyzer.cpp
    analysis/layer_analyzer.cpp
    analysis/module_analyzer.cpp
    analysis/naming_analyzer.cpp
    analysis/token_budget.cpp
    analysis/codebase_intelligence.cpp
    core/graph_propagator.cpp
    core/semantic_annotator.cpp
    core/context_lookup.cpp
    core/context_lookup_variables.cpp
    core/context_lookup_structure.cpp
    core/context_lookup_semantic.cpp
    symbollinker/linker_engine.cpp
    symbollinker/go_linker.cpp
    symbollinker/js_linker.cpp
    symbollinker/python_linker.cpp
    symbollinker/csharp_linker.cpp
    symbollinker/php_linker.cpp
    server/server.cpp
    server/handlers_search.cpp
    server/client.cpp
    mcp/server.cpp
    mcp/response.cpp
    mcp/validation.cpp
    mcp/formatter_compact.cpp
    mcp/handlers_core.cpp
    mcp/handlers_analysis.cpp
    mcp/handlers_explore.cpp
    mcp/handlers_index.cpp
    mcp/handlers_context.cpp
    mcp/context_manifest_expander.cpp
    cli/cli_core.cpp
    cli/search.cpp
    cli/status.cpp
    cli/server.cpp
    cli/mcp.cpp
    cli/commands.cpp
    cli/debug.cpp
    cli/update.cpp
    cli/profiling.cpp
    update/updater.cpp
    git/types.cpp
    git/provider.cpp
    git/pattern_detector.cpp
    git/analyzer.cpp
    git/serialize.cpp
    git/frequency_analyzer.cpp
)

target_include_directories(lci_lib PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/generated"
)

target_link_libraries(lci_lib PUBLIC
    nlohmann_json::nlohmann_json
    nlohmann_json_schema_validator
    httplib::httplib
    absl::flat_hash_map
    xxHash::xxhash
    tree-sitter
    tree-sitter-go
    tree-sitter-python
    tree-sitter-javascript
    tree-sitter-typescript
    tree-sitter-tsx
    tree-sitter-rust
    tree-sitter-c
    tree-sitter-cpp
    tree-sitter-java
    tree-sitter-c-sharp
    tree-sitter-php
    tree-sitter-kotlin
    tree-sitter-zig
    tree-sitter-ruby
    efsw
    rapidfuzz::rapidfuzz
    ${LCI_LIBSTEMMER_TARGET}
    re2::re2
)

# Platform-specific libraries
if(WIN32)
    target_link_libraries(lci_lib PUBLIC psapi)
endif()
# CoreServices framework no longer needed: efsw vendors its own FSEvents bindings.

# Optional: gperftools for --profile-cpu / --profile-memory wiring (DEV ONLY).
#
# OFF by default so release binaries stay self-contained. Auto-detecting the
# system gperftools and linking its SHARED libprofiler/libtcmalloc made the
# release binary hard-depend on libprofiler.so.0; machines without
# libgoogle-perftools installed fail at load ("error while loading shared
# libraries: libprofiler.so.0"). Enable explicitly for local profiling:
# -DLCI_ENABLE_GPERFTOOLS=ON. When disabled, profiling.cpp fails fast at
# runtime (Karpathy rule 6) with a clear rebuild hint.
#
# tcmalloc replaces the global allocator and is incompatible with the
# sanitizer allocators (ASan/TSan/MSan); linking both aborts at startup
# ("Attempt to free invalid pointer"). Force-disabled under any -fsanitize
# build so sanitizer presets (e.g. tsan) link and run even if ON is passed.
option(LCI_ENABLE_GPERFTOOLS "Link gperftools (shared libprofiler/libtcmalloc) for --profile-cpu/--profile-memory; dev only, breaks self-contained release" OFF)
string(FIND "${CMAKE_CXX_FLAGS}" "-fsanitize" LCI_SANITIZER_POS)
if(LCI_SANITIZER_POS GREATER -1)
    set(LCI_SANITIZER_BUILD ON)
else()
    set(LCI_SANITIZER_BUILD OFF)
endif()
if(NOT LCI_ENABLE_GPERFTOOLS)
    message(STATUS "gperftools disabled (LCI_ENABLE_GPERFTOOLS=OFF) - --profile-cpu/--profile-memory fail-fast at runtime")
elseif(LCI_SANITIZER_BUILD)
    message(STATUS "sanitizer build - gperftools/tcmalloc force-disabled (allocator conflict)")
else()
    find_library(LCI_GPERFTOOLS_PROFILER profiler)
    find_library(LCI_GPERFTOOLS_TCMALLOC tcmalloc)
    find_path(LCI_GPERFTOOLS_INCLUDE gperftools/profiler.h)
    if(LCI_GPERFTOOLS_PROFILER AND LCI_GPERFTOOLS_TCMALLOC AND LCI_GPERFTOOLS_INCLUDE)
        message(STATUS "gperftools found - enabling --profile-cpu / --profile-memory")
        target_compile_definitions(lci_lib PUBLIC LCI_HAVE_GPERFTOOLS=1)
        target_include_directories(lci_lib PUBLIC "${LCI_GPERFTOOLS_INCLUDE}")
        target_link_libraries(lci_lib PUBLIC
            "${LCI_GPERFTOOLS_PROFILER}"
            "${LCI_GPERFTOOLS_TCMALLOC}"
        )
    else()
        message(FATAL_ERROR "LCI_ENABLE_GPERFTOOLS=ON but gperftools not found "
            "(apt: libgoogle-perftools-dev, brew: gperftools, vcpkg: gperftools)")
    endif()
endif()

add_executable(lci
    cli/main.cpp
)

target_include_directories(lci PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/generated"
)

target_link_libraries(lci PRIVATE
    lci_lib
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    absl::strings
    absl::flat_hash_map
)

# Project warning flags (target-scoped, do not leak into FetchContent libs).
target_compile_options(lci_lib PRIVATE ${LCI_WARNING_FLAGS})
target_compile_options(lci PRIVATE ${LCI_WARNING_FLAGS})

# Precompiled headers. The heaviest, most widely-included headers — nlohmann/json
# (~25k lines, in 26 TUs) and absl flat_hash_map (39 TUs) — are parsed once into
# the PCH instead of per translation unit. Largest clean-build compile-time win
# for the library. Both deps are linked PUBLIC above, so their include dirs are
# already on lci_lib's compile line; the std headers are ubiquitous (<string> in
# 134 TUs, <vector> in 100).
target_precompile_headers(lci_lib PRIVATE
    <string>
    <string_view>
    <vector>
    <memory>
    <unordered_map>
    <nlohmann/json.hpp>
    <absl/container/flat_hash_map.h>
)
