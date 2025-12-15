cmake_minimum_required(VERSION 3.10)
project(Testing)

enable_testing()

add_executable(tests test_parser.cpp ../src/parser.cpp ../src/util.cpp)

target_link_libraries(tests gtest gtest_main)

add_test(NAME tests COMMAND tests)
