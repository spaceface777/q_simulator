cmake_minimum_required(VERSION 3.10)
project(test)

enable_testing()

add_executable(tests 
    test_parser.cpp ../src/events/parser.cpp 
    test_purple.cpp ../src/purple/purple.cpp
    ../src/util.cpp)

target_link_libraries(tests gtest gtest_main)
target_compile_definitions(tests PRIVATE TESTS)

add_test(NAME tests COMMAND tests)
