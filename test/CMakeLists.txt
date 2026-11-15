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
    sim/test_crash_containment.cpp
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

# Link the core that the world uses, already optimized, so the test link is an ordinary link.
if(sim_core_object)
    target_link_libraries(tests PRIVATE gtest core_interface "${sim_core_object}")
    add_dependencies(tests qsim-core)
    set_property(TARGET tests APPEND PROPERTY LINK_DEPENDS "${sim_core_object}")
else()
    target_link_libraries(tests PRIVATE gtest core)
endif()

# Resolve scenario fixtures independently of the test working directory.
target_compile_definitions(tests PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")

target_compile_options(tests PRIVATE ${COMPILATION_FLAGS} ${test_compile_options} -fno-omit-frame-pointer)

if(QUERCUS_COSMOPOLITAN AND CMAKE_HOST_APPLE)
    add_test(NAME tests COMMAND sh $<TARGET_FILE:tests>)
else()
    add_test(NAME tests COMMAND tests)
endif()
