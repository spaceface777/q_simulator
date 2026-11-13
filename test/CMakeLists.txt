cmake_minimum_required(VERSION 3.10)
project(test)

enable_testing()

add_executable(tests
    main.cpp
    test_settings.cpp
    synchronization/test_vclock.cpp 
    event/test_events.cpp
    ltl/test_parser.cpp
    ltl/test_monitor.cpp
    ltl/test_observation.cpp
    module/test_types.cpp
    sim/scripted_image.cpp
    sim/test_runtime.cpp
    sim/test_determinism.cpp
    rpc/test_frame.cpp
    seed/test_seed.cpp
    engine/test_task_queue.cpp
    net/test_network.cpp
    platform/test_overload.cpp
    platform/test_platform.cpp
    platform/test_rpc_worker_pool.cpp
    world/test_world.cpp
    world/test_instruments.cpp
    world/test_calibration_statistics.cpp
    world/test_servo_travel.cpp)

target_link_libraries(tests PRIVATE
    gtest
    core)

target_compile_definitions(core PRIVATE TESTING)
target_compile_definitions(tests PRIVATE TESTING)

# Resolve scenario fixtures independently of the test working directory.
target_compile_definitions(tests PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")

target_compile_options(tests PRIVATE ${COMPILATION_FLAGS})

add_test(NAME tests COMMAND tests)
