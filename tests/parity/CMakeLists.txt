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
find_program(LCI_GO_PATH NAMES lci-linux-amd64 lci HINTS
    /home/beagle/work/core/lci
    /usr/local/bin
    ENV PATH)

if(NOT LCI_GO_PATH)
    message(WARNING "LCI_GO_PATH not set and Go lci binary not found — side-by-side parity tests will be skipped")
else()
    message(STATUS "Parity: using Go binary at ${LCI_GO_PATH}")

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
        add_test(NAME parity.${TID}
                 COMMAND parity_runner ${D}
                 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
        set_tests_properties(parity.${TID} PROPERTIES
            LABELS "parity"
            FIXTURES_REQUIRED parity_fixture
            ENVIRONMENT "LCI_GO=${LCI_GO_PATH};LCI_CPP=$<TARGET_FILE:lci>;PARITY_CORPORA=${CMAKE_CURRENT_SOURCE_DIR}/corpora;PARITY_FAILURES=${CMAKE_BINARY_DIR}/parity-failures"
            TIMEOUT 120)
        if(D MATCHES ".*/descriptors/index/.*")
            set_tests_properties(parity.${TID} PROPERTIES
                TIMEOUT 420)
        endif()
        if(D MATCHES ".*/descriptors/http/.*")
            set_tests_properties(parity.${TID} PROPERTIES
                RESOURCE_LOCK "parity-http-socket")
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
