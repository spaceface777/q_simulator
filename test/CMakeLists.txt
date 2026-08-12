cmake_minimum_required(VERSION 3.10)
project(test)

enable_testing()

add_executable(tests 
    test_settings.cpp 
    synchronization/test_vclock.cpp 
    module/test_types.cpp
    childproc/test_launcher.cpp
    childsync/test_mailbox.cpp
    childsync/test_barrier.cpp
    rpc/test_frame.cpp
    seed/test_seed.cpp
    engine/test_task_queue.cpp
    net/test_network.cpp
    platform/test_overload.cpp
    platform/test_platform.cpp
    world/test_world.cpp
    world/test_instruments.cpp
    world/test_calibration_statistics.cpp
    world/test_servo_travel.cpp)

add_executable(fake_child sim/fake_child.cpp)
target_link_libraries(fake_child PRIVATE core)
target_compile_options(fake_child PRIVATE ${COMPILATION_FLAGS})

target_link_libraries(tests PRIVATE 
    gtest gtest_main
    core)

add_dependencies(tests fake_child)
target_compile_definitions(tests PRIVATE FAKE_CHILD_PATH="$<TARGET_FILE:fake_child>")

target_compile_definitions(core PRIVATE TESTING)
target_compile_definitions(tests PRIVATE TESTING)

target_compile_options(tests PRIVATE ${COMPILATION_FLAGS})

add_test(NAME tests COMMAND tests)
