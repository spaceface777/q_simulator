cmake_minimum_required(VERSION 3.10)
project(test)

enable_testing()

add_executable(tests 
    message/test_message.cpp 
    test_settings.cpp 
    synchronization/test_vclock.cpp 
    event/test_events.cpp
    module/test_types.cpp
    module/test_module.cpp
    tcp/test_socket.cpp
    tcp/test_server.cpp
    tub/test_tub.cpp
    tub/test_types.cpp)

target_link_libraries(tests PRIVATE 
    gtest gtest_main
    core)

target_compile_definitions(core PRIVATE TESTING)
target_compile_definitions(tests PRIVATE TESTING)

target_compile_options(tests PRIVATE ${COMPILATION_FLAGS})

add_test(NAME tests COMMAND tests)
