# tests/parity/CMakeLists.txt
# Parity harness: drives both Go and C++ lci binaries side-by-side.

# Canonicalization + tier-aware diff lives in tests/lib/spec_diff (built
# at the tests/ scope so the future integration suite can link it too).
# The parity runner just consumes spec_diff as its diff engine.

# ---- Runner library (subprocess + mode handlers) ----
# Always built — needed by integration tests even without Go binary.
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
    spec_diff
    nlohmann_json::nlohmann_json
    httplib::httplib
)
target_compile_features(lci_parity_runner_lib PUBLIC cxx_std_20)
target_compile_options(lci_parity_runner_lib PRIVATE ${LCI_WARNING_FLAGS})

# ---- Unit tests for the diff engine ----
add_executable(parity_unit_tests
    unit_tests/canonicalize_test.cpp
    unit_tests/field_tier_test.cpp
    unit_tests/diff_test.cpp
    unit_tests/descriptor_test.cpp
    unit_tests/socket_path_test.cpp
    unit_tests/ignore_baseline_test.cpp
)
target_link_libraries(parity_unit_tests PRIVATE
    spec_diff
    lci_parity_runner_lib
    GTest::gtest_main
)
include(GoogleTest)
target_compile_options(parity_unit_tests PRIVATE ${LCI_WARNING_FLAGS})
gtest_discover_tests(parity_unit_tests
    PROPERTIES
        LABELS "parity"
        ENVIRONMENT "PARITY_DESCRIPTORS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/descriptors"
)

# ---- Locate Go reference binary (for side-by-side parity runner) ----
if(NOT LCI_GO_PATH)
    set(LCI_GO_PATH "LCI_GO_PATH-NOTFOUND" CACHE FILEPATH
        "Path to Go reference lci binary for parity tests" FORCE)
endif()
find_program(LCI_GO_PATH NAMES lci-go-0.4.1 lci-linux-amd64 HINTS
    /home/beagle/work/core/lci
    /usr/local/bin
    ENV PATH)

# Parity tests run against either a live Go reference binary (LCI_GO_PATH)
# or frozen Go output snapshots in tests/parity/snapshots/. Either one is
# sufficient to register the tests; the runner prefers snapshots and falls
# back to Go on missing snapshot files.
set(PARITY_SNAPSHOTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/snapshots")
if(EXISTS "${PARITY_SNAPSHOTS_DIR}" AND IS_DIRECTORY "${PARITY_SNAPSHOTS_DIR}")
    set(PARITY_HAS_SNAPSHOTS ON)
    message(STATUS "Parity: using frozen snapshots at ${PARITY_SNAPSHOTS_DIR}")
else()
    set(PARITY_HAS_SNAPSHOTS OFF)
endif()

if(NOT LCI_GO_PATH AND NOT PARITY_HAS_SNAPSHOTS)
    message(WARNING "Neither LCI_GO_PATH nor frozen snapshots present — parity tests will be skipped")
else()
    if(LCI_GO_PATH)
        message(STATUS "Parity: Go reference binary at ${LCI_GO_PATH} (fallback)")
    endif()

    # ---- Runner executable ----
    add_executable(parity_runner runner/parity_runner.cpp)
    target_link_libraries(parity_runner PRIVATE
        lci_parity_runner_lib
        CLI11::CLI11
    )
    target_compile_options(parity_runner PRIVATE ${LCI_WARNING_FLAGS})
    add_dependencies(parity_runner lci)

    # ---- Per-descriptor parity tests ----
    file(GLOB_RECURSE PARITY_DESCS CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/descriptors/*.parity.json)

    foreach(D ${PARITY_DESCS})
        file(RELATIVE_PATH NAME ${CMAKE_CURRENT_SOURCE_DIR}/descriptors ${D})
        string(REPLACE "/" "." TID ${NAME})
        string(REPLACE ".parity.json" "" TID ${TID})

        # Extract the descriptor's corpus name so we can scope the
        # per-test RESOURCE_LOCK to that corpus only — see README
        # "Parallel execution" section for the rationale. file(READ) +
        # regex is enough; every descriptor has a literal "corpus":
        # "<name>" line near the top.
        file(READ ${D} DESC_CONTENT)
        if(DESC_CONTENT MATCHES "\"corpus\"[ \t]*:[ \t]*\"([^\"]+)\"")
            set(CORPUS_NAME "${CMAKE_MATCH_1}")
        else()
            set(CORPUS_NAME "unknown")
        endif()

        add_test(NAME parity.${TID}
                 COMMAND parity_runner ${D}
                 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
        set_tests_properties(parity.${TID} PROPERTIES
            LABELS "parity"
            FIXTURES_REQUIRED parity_fixture
            ENVIRONMENT "LCI_GO=${LCI_GO_PATH};LCI_CPP=$<TARGET_FILE:lci>;PARITY_CORPORA=${CMAKE_CURRENT_SOURCE_DIR}/corpora;PARITY_FAILURES=${CMAKE_BINARY_DIR}/parity-failures;PARITY_SNAPSHOTS=${CMAKE_CURRENT_SOURCE_DIR}/snapshots"
            TIMEOUT 120
            # Per-corpus serialization: every parity descriptor (cli /
            # mcp / http / index) ends up touching a server keyed on
            # corpus path — the C++ binary's CLI subcommands auto-spawn
            # or auto-connect to the per-corpus Unix socket (same hash
            # as HTTP mode), and `lci debug export` queries the running
            # server. Two tests against the same corpus therefore race
            # on socket bind/connect. Lock per corpus so different
            # corpora still run in parallel.
            RESOURCE_LOCK "parity-corpus-${CORPUS_NAME}")
        if(D MATCHES ".*/descriptors/index/.*")
            # index_export runs Go then C++ binaries serially, each capped
            # at 600s by run_index_export. Outer ctest timeout = 2×600
            # plus 120s slack for build/IO.
            set_tests_properties(parity.${TID} PROPERTIES
                TIMEOUT 1320)
        endif()
    endforeach()
endif()

# ---- Orphan-process cleanup fixtures ----
# CTest fixtures run setup before the suite and verify after, ensuring
# leaked lci server children from prior crashed runs don't pollute the
# next run (parity_runner_inotify_leak memory note). Tests in the parity
# label depend on parity_setup; parity_verify runs after them all.
add_test(NAME parity_setup
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/orphan_cleanup.sh setup)
set_tests_properties(parity_setup PROPERTIES
    FIXTURES_SETUP parity_fixture
    LABELS "parity")

add_test(NAME parity_verify
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/orphan_cleanup.sh verify)
set_tests_properties(parity_verify PROPERTIES
    FIXTURES_CLEANUP parity_fixture
    LABELS "parity")

# ---- Mask audit script as a CTest entry ----
# Runs scripts/mask_audit.py, regenerates MASK_AUDIT.md, and fails if any
# descriptor mask still falls into the (?) Unclassified bucket. Cheap;
# always part of the parity label.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME parity.mask_audit
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/scripts/mask_audit.py
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(parity.mask_audit PROPERTIES
        LABELS "parity"
        TIMEOUT 30)
endif()
