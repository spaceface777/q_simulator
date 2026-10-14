cmake_minimum_required(VERSION 3.10)
project(test)

enable_testing()

add_executable(tests
    main.cpp
    firmware/test_image.cpp
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

foreach(image counter wrong_abi missing_entry null_api)
    add_library(${image}_image MODULE firmware/counter.c)
    set_target_properties(${image}_image PROPERTIES PREFIX "" SUFFIX ".dll"
        C_VISIBILITY_PRESET hidden LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/firmware")
    target_compile_features(${image}_image PRIVATE c_std_11)
endforeach()
target_compile_definitions(wrong_abi_image PRIVATE COUNTER_ABI_VERSION=9)
target_compile_definitions(missing_entry_image PRIVATE COUNTER_MISSING_ENTRY)
target_compile_definitions(null_api_image PRIVATE COUNTER_NULL_API)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/firmware/not-image.dll" CONTENT "This is not a shared library.\n")
target_compile_definitions(tests PRIVATE COUNTER_IMAGE="$<TARGET_FILE:counter_image>"
    WRONG_ABI_IMAGE="$<TARGET_FILE:wrong_abi_image>" MISSING_ENTRY_IMAGE="$<TARGET_FILE:missing_entry_image>"
    NULL_API_IMAGE="$<TARGET_FILE:null_api_image>" NON_IMAGE="${CMAKE_CURRENT_BINARY_DIR}/firmware/not-image.dll")
add_dependencies(tests counter_image wrong_abi_image missing_entry_image null_api_image)
