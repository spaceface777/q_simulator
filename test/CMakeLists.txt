cmake_minimum_required(VERSION 3.10)
project(test)

enable_testing()

add_executable(tests 
    test_message.cpp 
    test_settings.cpp 
    test_vclock.cpp 
    test_events.cpp
    
    ../src/tcp/server.cpp 
    ../src/tcp/client.cpp 
    ../src/util.cpp 
    ../src/module/module.cpp
    ../src/message/message.cpp
    ../src/tub/tub.cpp
    ../src/settings.cpp
    ../src/event/events.cpp)

target_link_libraries(tests PRIVATE 
    gtest gtest_main
    tomlplusplus::tomlplusplus)

add_test(NAME tests COMMAND tests)
