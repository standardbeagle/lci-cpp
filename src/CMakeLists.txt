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
    core/postings.cpp
    core/import_resolver.cpp
    parser/parser.cpp
    parser/parser_pool.cpp
    parser/parse_methods.cpp
    parser/unified_extractor.cpp
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
    indexing/index_locks.cpp
    indexing/master_index.cpp
    indexing/master_index_search.cpp
    indexing/deleted_file_tracker.cpp
    indexing/debounced_rebuilder.cpp
    indexing/watcher.cpp
    regex_analyzer/engine.cpp
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
    analysis/metrics_calculator.cpp
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
    symbollinker/linker_engine.cpp
    symbollinker/go_linker.cpp
    symbollinker/js_linker.cpp
    symbollinker/python_linker.cpp
    symbollinker/csharp_linker.cpp
    symbollinker/php_linker.cpp
    server/server.cpp
    server/client.cpp
    mcp/server.cpp
    mcp/response.cpp
    mcp/validation.cpp
    mcp/pagination.cpp
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
    cli/profiling.cpp
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

# Optional: gperftools for --profile-cpu / --profile-memory wiring.
# When found, profiling.cpp uses ProfilerStart/HeapProfilerStart; when absent,
# the runtime path emits a clear error (Karpathy rule 6) instead of silent no-op.
#
# tcmalloc replaces the global allocator and is incompatible with the
# sanitizer allocators (ASan/TSan/MSan); linking both aborts at startup
# ("Attempt to free invalid pointer"). Skip gperftools entirely under any
# -fsanitize build so sanitizer presets (e.g. tsan) link and run.
string(FIND "${CMAKE_CXX_FLAGS}" "-fsanitize" LCI_SANITIZER_POS)
if(LCI_SANITIZER_POS GREATER -1)
    set(LCI_SANITIZER_BUILD ON)
else()
    set(LCI_SANITIZER_BUILD OFF)
endif()
find_library(LCI_GPERFTOOLS_PROFILER profiler)
find_library(LCI_GPERFTOOLS_TCMALLOC tcmalloc)
find_path(LCI_GPERFTOOLS_INCLUDE gperftools/profiler.h)
if(LCI_SANITIZER_BUILD)
    message(STATUS "sanitizer build - gperftools/tcmalloc disabled (allocator conflict)")
elseif(LCI_GPERFTOOLS_PROFILER AND LCI_GPERFTOOLS_TCMALLOC AND LCI_GPERFTOOLS_INCLUDE)
    message(STATUS "gperftools found - enabling --profile-cpu / --profile-memory")
    target_compile_definitions(lci_lib PUBLIC LCI_HAVE_GPERFTOOLS=1)
    target_include_directories(lci_lib PUBLIC "${LCI_GPERFTOOLS_INCLUDE}")
    target_link_libraries(lci_lib PUBLIC
        "${LCI_GPERFTOOLS_PROFILER}"
        "${LCI_GPERFTOOLS_TCMALLOC}"
    )
else()
    message(STATUS "gperftools not found - --profile-cpu/--profile-memory will fail-fast at runtime")
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
