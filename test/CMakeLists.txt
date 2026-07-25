cmake_minimum_required(VERSION 3.10)
project(test)

enable_testing()

add_executable(tests 
    test_message.cpp 
    test_settings.cpp 
    test_vclock.cpp 
    test_events.cpp)

target_link_libraries(tests PRIVATE 
    gtest gtest_main
    core)

add_test(NAME tests COMMAND tests)
