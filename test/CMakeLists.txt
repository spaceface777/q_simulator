cmake_minimum_required(VERSION 3.10)
project(test)

enable_testing()

add_executable(tests 
    message/test_message.cpp 
    test_settings.cpp 
    synchronization/test_vclock.cpp 
    event/test_events.cpp
    module/test_types.cpp
    tcp/test_socket.cpp
    tub/test_tub.cpp)

target_link_libraries(tests PRIVATE 
    gtest gtest_main
    core)

add_test(NAME tests COMMAND tests)
