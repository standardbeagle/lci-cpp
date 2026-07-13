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
    language_map_test.cpp
    types_test.cpp
    side_effects_test.cpp
    idcodec_test.cpp
    portable_test.cpp
    subprocess_test.cpp
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
    semantic_test.cpp
    side_effect_analyzer_test.cpp
    linker_engine_test.cpp
    go_js_linker_test.cpp
    python_csharp_linker_test.cpp
    php_integration_linker_test.cpp
    codebase_intelligence_test.cpp
    analysis_test.cpp
    call_graph_test.cpp
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
    updater_test.cpp
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

# The integration + real-project suites and the parity runner spawn child
# processes via fork/exec (tests/parity/runner/modes/*.cpp, child_guard.h) and
# only ever run on the Linux CI legs. They are gated to Linux: Windows has no
# fork/exec port, and on macOS they are never executed (the macOS leg runs only
# the unit suite) so compiling the un-run, not-yet-ported runner there buys
# nothing — and it trips libc++'s stricter transitive includes. The enforced
# Windows/macOS green bar is the unit suite (lci_tests) plus spec_diff tests.
# A runner port to Windows/macOS is tracked as a deferred follow-up.
if(NOT WIN32 AND NOT APPLE)

# ---------------------------------------------------------------------------
# Integration tests (non-real-project)
# ---------------------------------------------------------------------------
add_executable(lci_integration_tests
    integration/pipeline_integration_test.cpp
    integration/search_parity_test.cpp
    integration/server_lifecycle_test.cpp
    integration/mcp_tools_integration_test.cpp
    integration/spec_runner.cpp
    integration/spec_migration_test.cpp
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

# All integration spec_runner tests bundled into one ctest entry.
# Spec descriptors in http / mcp / cli modes spawn lci server children
# on /tmp/lci-<hash(corpus)>.sock — two parallel tests on the same
# corpus race on bind+connect, producing flaky golden-mismatch and
# wait_for_ready timeouts. Per-test ctest entries with RESOURCE_LOCK
# didn't fully serialize on this scheduler, and a per-test corpus
# symlink scheme broke the path-substitution layer in goldens. The
# bundle pattern matches lci_real_project_suite — single binary
# invocation, gtest reports its own pass/fail per case, ctest sees
# one exit code.
add_test(NAME lci_integration_suite
         COMMAND lci_integration_tests)
set_tests_properties(lci_integration_suite PROPERTIES
    LABELS "integration"
    ENVIRONMENT "LCI_CPP=$<TARGET_FILE:lci>;PARITY_CORPORA=${CMAKE_CURRENT_SOURCE_DIR}/parity/corpora"
    TIMEOUT 600
    # Spawns lci server children on per-corpus sockets. Without isolation it
    # races the parity suite's server spawns under -j (socket bind/connect
    # contention + subprocess pile-up), producing flaky golden-mismatch and
    # wait_for_ready timeouts. RUN_SERIAL runs it when no other test is
    # active, removing the cross-suite contention.
    RUN_SERIAL TRUE
)

# ---------------------------------------------------------------------------
# Real-project tests (bundled into one binary, run as ONE ctest entry so
# the in-process setup_real_project cache amortizes indexing across all
# TEST_F cases on the same corpus). Cuts integration wall by ~5× on the
# real-project subset (165s separate-process → 17s bundled).
# ---------------------------------------------------------------------------
add_executable(lci_real_project_tests
    integration/real_project_integration_test.cpp
    integration/real_project_analysis_test.cpp
    integration/real_project_context_test.cpp
    integration/real_project_phrase_test.cpp
    integration/real_project_performance_test.cpp
    integration/real_project_side_effects_test.cpp
    integration/real_project_typescript_test.cpp
    integration/real_project_feature_audit_test.cpp
    integration/real_project_languages_test.cpp
    integration/spec_runner.cpp
)
target_include_directories(lci_real_project_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(lci_real_project_tests PRIVATE
    LCI_TESTS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(lci_real_project_tests PRIVATE
    lci_lib
    lci_parity_runner_lib
    spec_diff
    GTest::gtest_main
)

add_test(NAME lci_real_project_suite
         COMMAND lci_real_project_tests)
set_tests_properties(lci_real_project_suite PROPERTIES
    LABELS "integration"
    ENVIRONMENT "LCI_CPP=$<TARGET_FILE:lci>;PARITY_CORPORA=${CMAKE_CURRENT_SOURCE_DIR}/parity/corpora"
    # 600s for the whole bundled suite; cache makes per-test cost near
    # zero after the first indexed corpus, so this is generous.
    TIMEOUT 600
    # Same rationale as lci_integration_suite: spawns lci server children;
    # serialize against other tests to avoid socket/subprocess contention.
    RUN_SERIAL TRUE
)

endif() # NOT WIN32 AND NOT APPLE

# ---------------------------------------------------------------------------
# Benchmark suite (indexing, search, trigram, parser, semantic)
# ---------------------------------------------------------------------------
# Gated to Linux: the benchmarks only ever run on the Linux self-hosted runner
# (the `benchmarks` CI job), and the sources use std::regex flag spellings
# (std::regex::multiline) that MSVC rejects and that are not worth validating on
# libc++ either. No Windows/macOS benchmark runner exists, so building them off
# Linux buys nothing.
if(NOT WIN32 AND NOT APPLE)
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
endif() # NOT WIN32 AND NOT APPLE

# ---------------------------------------------------------------------------
# Fuzz targets (Clang libFuzzer only)
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND LCI_ENABLE_FUZZING)
    # ASan/UBSan + SanitizerCoverage are applied build-wide by the `fuzz` preset
    # (CMAKE_CXX_FLAGS=-fsanitize=address,undefined,fuzzer-no-link) so that
    # lci_lib and every vendored dep (abseil, re2, ...) are instrumented
    # uniformly. That uniformity is required: mixing ASan-instrumented fuzz TUs
    # with a non-instrumented abseil trips abseil's container poisoning and
    # aborts with "CHECK failed: AddrIsInMem" the moment a flat_hash_map is
    # built (e.g. in Config()). Here the fuzz executables only add the libFuzzer
    # driver itself at link time; `fuzzer` must be a list element, not part of a
    # space-joined string, or clang folds it into one bad -fsanitize= value.
    set(FUZZ_LINK_FLAGS -fsanitize=fuzzer)

    # One recipe per target: include dirs (public headers + generated config),
    # link lci_lib, add the fuzzer driver, and the project warning flags. The
    # fuzz sources under fuzz/ share fuzz_fixture.h via "" include (compiler
    # searches the including file's dir first), so no extra include path needed.
    function(lci_add_fuzz_target name)
        add_executable(${name} fuzz/${name}.cpp)
        target_include_directories(${name} PRIVATE
            "${CMAKE_SOURCE_DIR}/include"
            "${CMAKE_BINARY_DIR}/generated"
        )
        target_link_libraries(${name} PRIVATE lci_lib)
        target_compile_options(${name} PRIVATE ${LCI_WARNING_FLAGS})
        target_link_options(${name} PRIVATE ${FUZZ_LINK_FLAGS})
    endfunction()

    # Narrow pure-logic targets (trigram, search-input) plus the four
    # complex-surface targets: the tree-sitter extraction engine, the full MCP
    # stdio/JSON-RPC boundary (all 14 tools + the unknown-param guard), the
    # find_files matcher, and the get_context id-codec / call-hierarchy path.
    set(LCI_FUZZ_TARGETS
        fuzz_trigram_extract
        fuzz_search_input
        fuzz_symbol_extract
        fuzz_mcp_dispatch
        fuzz_find_files
        fuzz_get_context)
    foreach(_ft IN LISTS LCI_FUZZ_TARGETS)
        lci_add_fuzz_target(${_ft})
    endforeach()

    # Register each target as a bounded ctest "smoke" run so the corpus is
    # actually exercised as a regression gate, not merely built — a non-zero
    # exit means a crash / ASan / UBSan finding. libFuzzer writes new inputs to
    # its first positional (corpus) arg, so point it at a throwaway dir under
    # the build tree and pass any committed seeds as a second, read-only input
    # dir; the checked-in seeds are never mutated. Short budget keeps CI fast;
    # deep runs use the same binary with a larger -max_total_time by hand.
    if(BUILD_TESTING)
        foreach(_ft IN LISTS LCI_FUZZ_TARGETS)
            set(_work "${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus/${_ft}")
            file(MAKE_DIRECTORY "${_work}")
            set(_positional "${_work}")
            set(_seed "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/${_ft}")
            if(EXISTS "${_seed}")
                list(APPEND _positional "${_seed}")
            endif()
            add_test(NAME ${_ft}_smoke
                COMMAND ${_ft} -runs=5000 -max_total_time=25 -timeout=10
                        -rss_limit_mb=4096 ${_positional})
            set_tests_properties(${_ft}_smoke PROPERTIES
                LABELS "fuzz"
                TIMEOUT 120
                ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
        endforeach()
    endif()
endif()

# Project warning flags applied per-test-target (matches src/CMakeLists.txt
# rationale: keep -Werror out of FetchContent vendored libs). Fuzz targets get
# the warning flags inside lci_add_fuzz_target above.
foreach(_t IN ITEMS lci_tests lci_helpers_tests lci_integration_tests lci_benchmarks)
    if(TARGET ${_t})
        target_compile_options(${_t} PRIVATE ${LCI_WARNING_FLAGS})
    endif()
endforeach()

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
# Parity runner is Linux-only (fork/exec; run only on the Linux CI legs).
if(NOT WIN32 AND NOT APPLE)
    add_subdirectory(parity)
endif()
