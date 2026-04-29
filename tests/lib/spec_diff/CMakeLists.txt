# tests/lib/spec_diff/CMakeLists.txt
#
# spec_diff: canonicalization + tier-aware structural diff library.
# Pure functions; no dependency on parity-runner concepts (binary paths,
# subprocess orchestration, descriptor file format) so it can be linked
# from both the parity oracle runner and the integration gtest suite.

add_library(spec_diff STATIC
    src/tiers.cpp
    src/canonicalize.cpp
    src/diff.cpp
    src/assert_matches.cpp
)
target_include_directories(spec_diff PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(spec_diff PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(spec_diff PUBLIC cxx_std_20)

# ---- Unit tests for the spec_diff library ----
# Built only when the gtest target is available (i.e. when tests/CMakeLists.txt
# has run and pulled in googletest via FetchContent).
if(TARGET GTest::gtest_main)
    add_executable(spec_diff_unit_tests
        tests/tiers_test.cpp
        tests/canonicalize_test.cpp
        tests/diff_test.cpp
        tests/assert_matches_test.cpp
    )
    target_link_libraries(spec_diff_unit_tests PRIVATE
        spec_diff
        GTest::gtest_main
    )
    include(GoogleTest)
    gtest_discover_tests(spec_diff_unit_tests
        PROPERTIES LABELS "spec_diff"
    )
endif()
