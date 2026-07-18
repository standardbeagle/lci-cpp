file(READ "${CTEST_FILE}" ctest_contents)

foreach(test_name IN ITEMS lci_benchmarks lci_real_project_suite)
    string(REGEX MATCH
        "set_tests_properties\\(\\[=\\[${test_name}\\]=\\].*RUN_SERIAL \"TRUE\""
        isolated "${ctest_contents}")
    if(NOT isolated)
        message(FATAL_ERROR
            "${test_name} must be RUN_SERIAL: both bundled tails saturate CPU and "
            "consume multiple GiB, so overlap makes their wall time load-dependent")
    endif()
endforeach()
