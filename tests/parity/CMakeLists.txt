# tests/parity/CMakeLists.txt
# Parity harness: drives both Go and C++ lci binaries side-by-side.

# Locate Go reference binary.
# `find_program` is a no-op if LCI_GO_PATH already has a non-NOTFOUND cache value,
# so user overrides via -DLCI_GO_PATH=... are preserved automatically.
if(NOT LCI_GO_PATH)
    set(LCI_GO_PATH "LCI_GO_PATH-NOTFOUND" CACHE FILEPATH
        "Path to Go reference lci binary for parity tests" FORCE)
endif()
find_program(LCI_GO_PATH NAMES lci-linux-amd64 lci HINTS
    /home/beagle/work/core/lci
    /usr/local/bin
    ENV PATH)

if(NOT LCI_GO_PATH)
    message(WARNING "LCI_GO_PATH not set and Go lci binary not found — parity tests will be skipped")
    return()
endif()
message(STATUS "Parity: using Go binary at ${LCI_GO_PATH}")

# ---- Diff engine library (pure functions, unit-testable) ----
add_library(lci_parity_diff STATIC
    diff_engine/canonicalize.cpp
    diff_engine/field_tier.cpp
    diff_engine/diff.cpp
)
target_include_directories(lci_parity_diff PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(lci_parity_diff PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(lci_parity_diff PUBLIC cxx_std_20)

# ---- Runner library (subprocess + mode handlers) ----
add_library(lci_parity_runner_lib STATIC
    runner/descriptor.cpp
    runner/modes/subst.cpp
    runner/modes/cli.cpp
    runner/modes/mcp.cpp
    runner/modes/http.cpp
    runner/modes/index.cpp
)
target_include_directories(lci_parity_runner_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(lci_parity_runner_lib PUBLIC
    lci_parity_diff
    nlohmann_json::nlohmann_json
    httplib::httplib
)

# ---- Runner executable ----
add_executable(parity_runner runner/parity_runner.cpp)
target_link_libraries(parity_runner PRIVATE
    lci_parity_runner_lib
    CLI11::CLI11
)
add_dependencies(parity_runner lci)

# ---- Unit tests for the diff engine ----
add_executable(parity_unit_tests
    unit_tests/canonicalize_test.cpp
    unit_tests/field_tier_test.cpp
    unit_tests/diff_test.cpp
    unit_tests/descriptor_test.cpp
)
target_link_libraries(parity_unit_tests PRIVATE
    lci_parity_diff
    lci_parity_runner_lib
    GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(parity_unit_tests
    PROPERTIES LABELS "parity"
)

# ---- Per-descriptor parity tests ----
file(GLOB_RECURSE PARITY_DESCS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/descriptors/*.parity.json)

foreach(D ${PARITY_DESCS})
    file(RELATIVE_PATH NAME ${CMAKE_CURRENT_SOURCE_DIR}/descriptors ${D})
    string(REPLACE "/" "." TID ${NAME})
    string(REPLACE ".parity.json" "" TID ${TID})
    add_test(NAME parity.${TID}
             COMMAND parity_runner ${D}
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(parity.${TID} PROPERTIES
        LABELS "parity"
        ENVIRONMENT "LCI_GO=${LCI_GO_PATH};LCI_CPP=$<TARGET_FILE:lci>;PARITY_CORPORA=${CMAKE_CURRENT_SOURCE_DIR}/corpora;PARITY_FAILURES=${CMAKE_BINARY_DIR}/parity-failures"
        TIMEOUT 120)
    # Index mode runs both binaries' debug-export, which re-indexes the
    # whole corpus from scratch. Go's debug-export on the Go-self repo
    # takes ~4m15s (no server reuse, no parallelism). Give index/*
    # descriptors a 420s outer (CTest) budget; the parity_runner already
    # enforces a 360s per-binary cap internally (so go+cpp = up to ~720s
    # worst case but well under the 420s wall thanks to C++ being instant).
    if(D MATCHES ".*/descriptors/index/.*")
        set_tests_properties(parity.${TID} PROPERTIES
            TIMEOUT 420)
    endif()
    # HTTP descriptors share /tmp/lci-server-<hash>.sock per corpus.
    # Serialize them to avoid spawn races. Probe descriptors and other
    # modes don't need this.
    if(D MATCHES ".*/descriptors/http/.*")
        # Simpler: use one global lock for all http tests; HTTP suite is small.
        set_tests_properties(parity.${TID} PROPERTIES
            RESOURCE_LOCK "parity-http-socket")
    endif()
endforeach()
