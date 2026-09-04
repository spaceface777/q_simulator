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
    childproc/test_launcher.cpp
    childsync/test_mailbox.cpp
    childsync/test_barrier.cpp
    rpc/test_frame.cpp
    seed/test_seed.cpp
    engine/test_task_queue.cpp
    net/test_network.cpp
    platform/test_overload.cpp
    platform/test_platform.cpp
    platform/test_rpc_worker_pool.cpp
    sim/test_applet_e2e.cpp
    sim/test_determinism.cpp
    sim/test_runtime.cpp
	    sim/test_runahead.cpp
    sim/test_scenarios.cpp
    sim/test_speed_invariance.cpp
    world/test_world.cpp
    world/test_instruments.cpp
    world/test_calibration_statistics.cpp
    world/test_servo_travel.cpp)

add_executable(fake_child sim/fake_child.cpp)
target_link_libraries(fake_child PRIVATE core)
target_compile_options(fake_child PRIVATE ${COMPILATION_FLAGS})

target_link_libraries(tests PRIVATE
    gtest
    core)

add_dependencies(tests fake_child)
target_compile_definitions(tests PRIVATE FAKE_CHILD_PATH="$<TARGET_FILE:fake_child>")

target_compile_definitions(core PRIVATE TESTING)
target_compile_definitions(tests PRIVATE TESTING)

# Lets test/sim/test_scenarios.cpp find scenarios/, layouts/ and settings_example.toml regardless
# of the working directory ctest or a manual ./build/tests invocation happens to run from.
target_compile_definitions(tests PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")

target_compile_options(tests PRIVATE ${COMPILATION_FLAGS})

add_test(NAME tests COMMAND tests)
