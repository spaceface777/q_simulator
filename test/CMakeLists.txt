add_executable(tests
    main.cpp test_settings.cpp firmware/test_image.cpp firmware/test_backend.cpp
    synchronization/test_vclock.cpp
    event/test_events.cpp
    ltl/test_parser.cpp ltl/test_monitor.cpp ltl/test_observation.cpp
    module/test_types.cpp rpc/test_frame.cpp seed/test_seed.cpp
    sim/test_embedded_environment.cpp sim/test_runtime.cpp sim/test_determinism.cpp sim/test_scenarios.cpp sim/test_applet_e2e.cpp sim/test_speed_invariance.cpp
    engine/test_task_queue.cpp net/test_network.cpp
    platform/test_overload.cpp platform/test_platform.cpp platform/test_rpc_worker_pool.cpp
    world/test_world.cpp world/test_instruments.cpp world/test_calibration_statistics.cpp world/test_servo_travel.cpp)
if(WIN32)
    target_sources(tests PRIVATE os_windows.cpp)
else()
    target_sources(tests PRIVATE os_posix.cpp)
endif()
target_link_libraries(tests PRIVATE gtest core)
target_compile_definitions(core PRIVATE TESTING)
target_compile_definitions(tests PRIVATE TESTING REPO_ROOT="${CMAKE_SOURCE_DIR}")
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
add_library(scripted_image MODULE sim/scripted_image.cpp "${CMAKE_SOURCE_DIR}/src/rpc/frame.cpp")
set_target_properties(scripted_image PROPERTIES PREFIX "" SUFFIX ".dll" CXX_VISIBILITY_PRESET hidden)
target_include_directories(scripted_image PRIVATE "${QUERCUS_SDK_ROOT}/include")
target_compile_options(scripted_image PRIVATE ${COMPILATION_FLAGS})
target_compile_definitions(tests PRIVATE SCRIPTED_IMAGE="$<TARGET_FILE:scripted_image>"
    QSIM_TEST_FIRMWARE_DIR="${QSIM_TEST_FIRMWARE_DIR}" QSIM_TEST_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/out")
add_dependencies(tests scripted_image)

option(QSIM_TEST_EMBEDDED_TRACE "Compare two tub_copier images with the legacy futex/pthread trace" OFF)
if(QSIM_TEST_EMBEDDED_TRACE)
    target_sources(tests PRIVATE sim/test_embedded_trace.cpp)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME sdk-consistency COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_sdk_consistency.py")
