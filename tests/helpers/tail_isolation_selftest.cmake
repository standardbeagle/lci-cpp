# Self-test for tail_isolation_test.cmake: proves the gate both accepts every
# supported CTest quoting form and REJECTS the cross-call false pass a greedy
# regex used to allow (RUN_SERIAL on a later, unrelated test).
#
# Run: cmake -P tail_isolation_selftest.cmake  (registered as
# tail_bundle_isolation_selftest in tests/CMakeLists.txt)

set(gate "${CMAKE_CURRENT_LIST_DIR}/tail_isolation_test.cmake")
set(work "${CMAKE_CURRENT_BINARY_DIR}/tail-isolation-selftest")
file(MAKE_DIRECTORY "${work}")

function(expect_gate fixture_name content expected_result)
    set(fixture "${work}/${fixture_name}.cmake")
    file(WRITE "${fixture}" "${content}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DCTEST_FILE=${fixture} -P "${gate}"
        RESULT_VARIABLE rc
        OUTPUT_QUIET ERROR_QUIET)
    if(expected_result STREQUAL "pass" AND NOT rc EQUAL 0)
        message(FATAL_ERROR "gate rejected valid fixture ${fixture_name}")
    endif()
    if(expected_result STREQUAL "fail" AND rc EQUAL 0)
        message(FATAL_ERROR "gate false-passed fixture ${fixture_name}")
    endif()
endfunction()

# Both tails serialized, CTest bracket quoting (current CMake output form).
expect_gate(good_bracket "
add_test([=[lci_benchmarks]=] \"/x/bench\")
set_tests_properties([=[lci_benchmarks]=] PROPERTIES LABELS \"perf\" RUN_SERIAL \"TRUE\" TIMEOUT \"600\")
set_tests_properties([=[lci_real_project_suite]=] PROPERTIES RUN_SERIAL \"TRUE\")
" pass)

# Same contract under plain double-quoted names/values: a CMake upgrade that
# drops bracket quoting must not hard-fail the gate.
expect_gate(good_plain "
set_tests_properties(\"lci_benchmarks\" PROPERTIES RUN_SERIAL TRUE)
set_tests_properties(lci_real_project_suite PROPERTIES RUN_SERIAL \"TRUE\")
" pass)

# The historical false pass: lci_benchmarks lacks RUN_SERIAL, but a LATER
# unrelated test carries it. A match that crosses call boundaries passes this.
expect_gate(bad_cross_call "
set_tests_properties([=[lci_benchmarks]=] PROPERTIES TIMEOUT \"600\")
set_tests_properties([=[lci_real_project_suite]=] PROPERTIES RUN_SERIAL \"TRUE\")
set_tests_properties([=[some_other_test]=] PROPERTIES RUN_SERIAL \"TRUE\")
" fail)

# Missing entirely must still fail.
expect_gate(bad_missing "
set_tests_properties([=[lci_benchmarks]=] PROPERTIES RUN_SERIAL \"TRUE\")
" fail)

file(REMOVE_RECURSE "${work}")
message(STATUS "tail_isolation_selftest: 4 fixtures behaved as expected")
