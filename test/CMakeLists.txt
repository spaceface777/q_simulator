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
    world/test_servo_travel.cpp
    trace/test_trace.cpp)

# The same name everywhere. cosmocc's x86-64 driver also makes an APE file only of *.exe outputs.
set_target_properties(tests PROPERTIES SUFFIX .exe)

# Link the core that the world uses, already optimized, so the test link is an ordinary link.
target_link_libraries(tests PRIVATE gtest core_interface "${sim_core_object}")
add_dependencies(tests qsim-core)
set_property(TARGET tests APPEND PROPERTY LINK_DEPENDS "${sim_core_object}")

target_compile_options(tests PRIVATE ${COMPILATION_FLAGS} ${test_compile_options} -fno-omit-frame-pointer)

# === Tests ===
# ctest names every test by its group: unit.<suite>, world.<check> and applet.<applet>.<scenario>.
# Select a group with -L unit, -L world or -L applet, and single tests with -R.

if(QUERCUS_COSMOPOLITAN AND NOT CMAKE_HOST_WIN32 AND (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR CMAKE_HOST_APPLE))
    # These builds make tests.exe an APE file, which starts only through a shell outside Windows. On
    # Linux arm64 it is ELF, which starts directly. The system's shell: CMake starts it directly, and
    # macOS cannot, when the toolchain's own sh (itself an APE file) comes first on PATH.
    set(unit_emulator /bin/sh)
endif()
set(unit_suites "${CMAKE_CURRENT_BINARY_DIR}/unit_suites.cmake")
if(NOT EXISTS "${unit_suites}")
    file(WRITE "${unit_suites}" "add_test(unit.NOT_BUILT \"${CMAKE_COMMAND}\" -E false)\n")
endif()
add_custom_command(TARGET tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DEMULATOR=${unit_emulator}" "-DEXECUTABLE=$<TARGET_FILE:tests>"
        "-DOUTPUT=${unit_suites}" -P "${CMAKE_CURRENT_SOURCE_DIR}/unit_suites.cmake"
    VERBATIM)
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${unit_suites}")

# The world and applet tests link the test applets into worlds. qbuild builds them first, for the
# simulator's toolchain: Zephyr and west must be set up as for any other Core 1 application.
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
set(applets_dir "${CMAKE_SOURCE_DIR}/test/applets")
if(QUERCUS_COSMOPOLITAN)
    set(applet_toolchain cosmocc)
    set(firmware_dir "${applets_dir}/out")
else()
    set(applet_toolchain host)
    set(firmware_dir "${applets_dir}/out/host")
endif()
add_test(NAME applet.build
    COMMAND "${Python3_EXECUTABLE}" "${applets_dir}/q_sdk/qbuild" sim --toolchain ${applet_toolchain}
    WORKING_DIRECTORY "${applets_dir}")
set_tests_properties(applet.build PROPERTIES FIXTURES_SETUP applets LABELS applet TIMEOUT 3600)

foreach(check repeatability command_line gui)
    add_test(NAME world.${check}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_worlds.py"
            "${CMAKE_BINARY_DIR}/qsim" "${firmware_dir}" ${check})
    set_tests_properties(world.${check} PROPERTIES FIXTURES_REQUIRED applets LABELS world
        SKIP_RETURN_CODE 77)
endforeach()

file(GLOB scenarios LIST_DIRECTORIES true CONFIGURE_DEPENDS "${applets_dir}/*/scenarios/*")
foreach(scenario IN LISTS scenarios)
    if(NOT EXISTS "${scenario}/expected.txt")
        continue()
    endif()
    get_filename_component(scenario_name "${scenario}" NAME)
    get_filename_component(applet "${scenario}/../.." ABSOLUTE)
    get_filename_component(applet "${applet}" NAME)
    add_test(NAME applet.${applet}.${scenario_name}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_scenario.py"
            "${CMAKE_BINARY_DIR}/qsim" "${firmware_dir}" "${scenario}")
    set_tests_properties(applet.${applet}.${scenario_name} PROPERTIES FIXTURES_REQUIRED applets LABELS applet)
endforeach()
