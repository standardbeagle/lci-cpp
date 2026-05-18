include(FetchContent)

FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Google Benchmark
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googlebenchmark)

# ---------------------------------------------------------------------------
# Unit tests (existing)
# ---------------------------------------------------------------------------
add_executable(lci_tests
    version_test.cpp
    types_test.cpp
    side_effects_test.cpp
    idcodec_test.cpp
    config_test.cpp
    gitignore_test.cpp
    error_string_pool_test.cpp
    slab_allocator_test.cpp
    file_content_store_test.cpp
    line_scanner_file_service_test.cpp
    parser_pool_test.cpp
    unified_extractor_test.cpp
    language_extraction_test.cpp
    symbol_store_test.cpp
    trigram_test.cpp
    reference_tracker_test.cpp
    pipeline_test.cpp
    integrator_test.cpp
    master_index_test.cpp
    master_index_search_test.cpp
    watcher_test.cpp
    search_engine_test.cpp
    regex_analyzer_test.cpp
    semantic_test.cpp
    side_effect_analyzer_test.cpp
    linker_engine_test.cpp
    go_js_linker_test.cpp
    python_csharp_linker_test.cpp
    php_integration_linker_test.cpp
    codebase_intelligence_test.cpp
    analysis_test.cpp
    server_test.cpp
    client_test.cpp
    mcp_server_test.cpp
    mcp_validation_pagination_test.cpp
    mcp_handlers_core_test.cpp
    mcp_handlers_analysis_test.cpp
    mcp_handlers_explore_index_test.cpp
    mcp_handlers_context_test.cpp
    cli_test.cpp
    git_test.cpp
    index_performance_test.cpp
    json_schema_smoketest.cpp
)
target_include_directories(lci_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/helpers"
)
target_compile_definitions(lci_tests PRIVATE
    LCI_TESTS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(lci_tests PRIVATE
    lci_lib
    nlohmann_json_schema_validator
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(lci_tests
    PROPERTIES
        LABELS "unit"
        # 60s per-test cap. ServerTest fixtures spawn IndexServer instances
        # that have hung indefinitely under parallel ctest load (pre-existing
        # parallel-flake class). Without a cap, one hang blocks the run.
        TIMEOUT 60
)

# ---------------------------------------------------------------------------
# Test helpers infrastructure tests
# ---------------------------------------------------------------------------
add_executable(lci_helpers_tests
    helpers/test_helpers_test.cpp
)
target_include_directories(lci_helpers_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/helpers"
)
target_link_libraries(lci_helpers_tests PRIVATE
    GTest::gtest_main
)

gtest_discover_tests(lci_helpers_tests
    PROPERTIES LABELS "infrastructure"
)

# ---------------------------------------------------------------------------
# Integration tests
# ---------------------------------------------------------------------------
add_executable(lci_integration_tests
    integration/pipeline_integration_test.cpp
    integration/search_parity_test.cpp
    integration/server_lifecycle_test.cpp
    integration/mcp_tools_integration_test.cpp
    integration/spec_runner.cpp
    integration/spec_migration_test.cpp
    integration/real_project_integration_test.cpp
    integration/real_project_analysis_test.cpp
    integration/real_project_context_test.cpp
    integration/real_project_phrase_test.cpp
    integration/real_project_performance_test.cpp
)
target_include_directories(lci_integration_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(lci_integration_tests PRIVATE
    LCI_TESTS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(lci_integration_tests PRIVATE
    lci_lib
    lci_parity_runner_lib
    spec_diff
    GTest::gtest_main
)

gtest_discover_tests(lci_integration_tests
    PROPERTIES
        LABELS "integration"
        ENVIRONMENT "LCI_CPP=$<TARGET_FILE:lci>;PARITY_CORPORA=${CMAKE_CURRENT_SOURCE_DIR}/parity/corpora"
        # 180s cap. Real-project indexing tests (chi/pocketbase/fastapi)
        # legitimately need 30-120s each on cold cache; a 60s cap was
        # killing them mid-index. ServerLifecycle hang class still needs
        # a real fix; 180s lets the legit slow path finish.
        TIMEOUT 180
)

# ---------------------------------------------------------------------------
# Benchmark suite (indexing, search, trigram, parser, semantic)
# ---------------------------------------------------------------------------
add_executable(lci_benchmarks
    benchmarks/benchmark_main.cpp
    benchmarks/real_project_benchmarks.cpp
    benchmarks/real_project_advanced_benchmarks.cpp
)
target_include_directories(lci_benchmarks PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(lci_benchmarks PRIVATE
    lci_lib
    benchmark::benchmark
    benchmark::benchmark_main
)

# CTest labels for benchmarks (run separately from unit tests)
add_test(NAME lci_benchmarks COMMAND lci_benchmarks --benchmark_min_time=0.01s)
set_tests_properties(lci_benchmarks PROPERTIES LABELS "benchmark")

# ---------------------------------------------------------------------------
# Fuzz targets (Clang libFuzzer only)
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND LCI_ENABLE_FUZZING)
    # Must be CMake lists (not space-joined strings): target_compile_options
    # passes a quoted string as a single compiler token, which makes clang
    # parse "-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer" as
    # one -fsanitize= value and reject it.
    set(FUZZ_LINK_FLAGS -fsanitize=fuzzer,address,undefined)
    set(FUZZ_COMPILE_FLAGS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)

    add_executable(fuzz_regex_classifier fuzz/fuzz_regex_classifier.cpp)
    target_include_directories(fuzz_regex_classifier PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_BINARY_DIR}/generated"
    )
    target_link_libraries(fuzz_regex_classifier PRIVATE lci_lib)
    target_compile_options(fuzz_regex_classifier PRIVATE ${FUZZ_COMPILE_FLAGS})
    target_link_options(fuzz_regex_classifier PRIVATE ${FUZZ_LINK_FLAGS})

    add_executable(fuzz_trigram_extract fuzz/fuzz_trigram_extract.cpp)
    target_include_directories(fuzz_trigram_extract PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_BINARY_DIR}/generated"
    )
    target_link_libraries(fuzz_trigram_extract PRIVATE lci_lib)
    target_compile_options(fuzz_trigram_extract PRIVATE ${FUZZ_COMPILE_FLAGS})
    target_link_options(fuzz_trigram_extract PRIVATE ${FUZZ_LINK_FLAGS})

    add_executable(fuzz_search_input fuzz/fuzz_search_input.cpp)
    target_include_directories(fuzz_search_input PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_BINARY_DIR}/generated"
    )
    target_link_libraries(fuzz_search_input PRIVATE lci_lib)
    target_compile_options(fuzz_search_input PRIVATE ${FUZZ_COMPILE_FLAGS})
    target_link_options(fuzz_search_input PRIVATE ${FUZZ_LINK_FLAGS})
endif()

# Project warning flags applied per-test-target (matches src/CMakeLists.txt
# rationale: keep -Werror out of FetchContent vendored libs).
foreach(_t IN ITEMS lci_tests lci_helpers_tests lci_integration_tests lci_benchmarks)
    if(TARGET ${_t})
        target_compile_options(${_t} PRIVATE ${LCI_WARNING_FLAGS})
    endif()
endforeach()
if(LCI_FUZZ)
    foreach(_t IN ITEMS fuzz_regex_classifier fuzz_trigram_extract fuzz_search_input)
        if(TARGET ${_t})
            target_compile_options(${_t} PRIVATE ${LCI_WARNING_FLAGS})
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Copy fixture files to build directory for runtime access
# ---------------------------------------------------------------------------
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/"
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/fixtures/"
)

# Reusable test-side libraries (consumed by parity harness AND future
# integration gtests). Must be added BEFORE tests/parity so the parity
# build can link the spec_diff target.
add_subdirectory(lib/spec_diff)
add_subdirectory(parity)
