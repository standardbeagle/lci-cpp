# tests/parity/CMakeLists.txt
#
# The Go reference binary has been retired — the migration is complete and LCI
# is its own product. What remains here is C++-only test infrastructure:
#   * lci_parity_runner_lib — the spec-driver (subprocess + cli/mcp/http/index
#     mode handlers) that the integration suite uses to run the C++ binary and
#     diff its output against committed goldens.
#   * parity_unit_tests — unit tests for the canonicalization + tier-aware diff
#     engine (spec_diff) and the descriptor parser.
#   * corpora/ — shared test corpora (also consumed by the integration suite).

# ---- Spec-runner library (subprocess + mode handlers) ----
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

# ---- Unit tests for the diff engine + descriptor parser ----
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
        LABELS "diff")
