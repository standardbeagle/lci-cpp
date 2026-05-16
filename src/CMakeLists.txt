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
    semantic/vocabulary_analyzer.cpp
    analysis/side_effect_analyzer.cpp
    analysis/metrics_calculator.cpp
    analysis/health_analyzer.cpp
    analysis/ci_vocabulary_analyzer.cpp
    analysis/coupling_analyzer.cpp
    analysis/feature_analyzer.cpp
    analysis/layer_analyzer.cpp
    analysis/module_analyzer.cpp
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
)

# Platform-specific libraries
if(WIN32)
    target_link_libraries(lci_lib PUBLIC psapi)
endif()
# CoreServices framework no longer needed: efsw vendors its own FSEvents bindings.

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
