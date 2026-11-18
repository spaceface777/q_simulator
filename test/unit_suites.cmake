# Registers one ctest test for each GoogleTest suite of the unit test executable, as unit.<suite>.
# The tests target runs this after each link, so new suites need no reconfigure.
# Inputs: EXECUTABLE, OUTPUT (the ctest include file to write).
execute_process(COMMAND "${EXECUTABLE}" --gtest_list_tests
    OUTPUT_VARIABLE listing RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Could not list the unit tests: ${result}")
endif()
# Suite lines are the unindented ones, like "Suite." or "Prefix/Suite/0.  # TypeParam = int".
string(REGEX MATCHALL "(^|\n)[^ \n#][^ \n]*\\." suites "${listing}")
set(content "")
foreach(suite IN LISTS suites)
    string(REGEX REPLACE "^\n?(.*)\\.$" "\\1" suite "${suite}")
    string(APPEND content "add_test([=[unit.${suite}]=] [=[${EXECUTABLE}]=] [=[--gtest_filter=${suite}.*]=])\n"
        "set_tests_properties([=[unit.${suite}]=] PROPERTIES LABELS unit)\n")
endforeach()
file(WRITE "${OUTPUT}" "${content}")
