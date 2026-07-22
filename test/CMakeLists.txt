cmake_minimum_required(VERSION 3.10)
project(test)

enable_testing()

add_executable(tests 
    test_message.cpp 
    test_settings.cpp 
    test_vclock.cpp 
    
    ../src/message/message.cpp
    ../src/settings.cpp
    ../src/util.cpp)

target_link_libraries(tests gtest gtest_main)
target_compile_definitions(tests PRIVATE TESTS)

add_test(NAME tests COMMAND tests)
