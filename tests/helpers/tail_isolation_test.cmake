file(READ "${CTEST_FILE}" ctest_contents)

foreach(test_name IN ITEMS lci_benchmarks lci_real_project_suite)
    # Scope the match to this test's own set_tests_properties(<name> PROPERTIES
    # ... ) call: [^)]* cannot cross the closing parenthesis, so RUN_SERIAL on
    # some LATER test can never satisfy this test's requirement (a greedy .*
    # matched to end-of-file and false-passed exactly that way). Accept both
    # CTest spellings for the name and the value — bracket-quoted [=[...]=] /
    # [==[...]==] or plain/double-quoted — so a CMake quoting change cannot
    # hard-fail the gate on syntax alone.
    string(REGEX MATCH
        "set_tests_properties\\((\\[=*\\[)?\"?${test_name}(\\]=*\\])?\"? PROPERTIES[^)]*RUN_SERIAL \"?(\\[=*\\[)?TRUE"
        isolated "${ctest_contents}")
    if(NOT isolated)
        message(FATAL_ERROR
            "${test_name} must be RUN_SERIAL: both bundled tails saturate CPU and "
            "consume multiple GiB, so overlap makes their wall time load-dependent")
    endif()
endforeach()
