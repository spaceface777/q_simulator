cmake_minimum_required(VERSION 3.12.0)

# Build with cosmocc unless the configure names another toolchain file or asks for the host compiler.
option(QSIM_COSMOCC "Build with the cosmocc toolchain (OFF: host compiler, Linux only)" ON)
if(QSIM_COSMOCC AND NOT DEFINED CMAKE_TOOLCHAIN_FILE AND NOT DEFINED ENV{CMAKE_TOOLCHAIN_FILE})
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cosmocc.cmake" CACHE FILEPATH "Toolchain file")
endif()

project(simulation VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_CXX_STANDARD 23)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(COMPILATION_FLAGS -Wall -Wextra -Werror)
if(QUERCUS_COSMOPOLITAN)
    # GCC 16 can't tell that libc++'s hash table only reads its bucket hash when it has buckets :((
    list(APPEND COMPILATION_FLAGS -Wno-error=maybe-uninitialized)
    if(CMAKE_HOST_APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        # The AArch64 driver emits ELF. Wrap each executable for the Apple Silicon loader.
        set(cosmocc_bin "${COSMOCC_TOOLCHAIN_PATH}/bin")
        # Each list item is one command, which both the Ninja and Makefile generators support.
        list(APPEND CMAKE_CXX_LINK_EXECUTABLE
            "\"${CMAKE_COMMAND}\" -E rename <TARGET> <TARGET>.dbg"
            "sh \"${cosmocc_bin}/apelink\" -l \"${cosmocc_bin}/ape-aarch64.elf\" -M \"${cosmocc_bin}/ape-m1.c\" -o <TARGET> <TARGET>.dbg")
    endif()
endif()
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -g")
option(ENABLE_LTO "Enable LTO in optimized builds" ON)
# The prelink below needs GCC's -flinker-output; other compilers build without LTO.
if(ENABLE_LTO AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Only the core carries LTO bytecode. The qsim-core.o prelink below optimizes it once, and both
    # the world entry point and the tests link that result.
    set(sim_lto $<$<CONFIG:Release>:-flto=auto>)
    set(prelink_lto -flto=auto -flinker-output=nolto-rel)
endif()

# Test code and GoogleTest skip LTO and the expensive -O3 passes. -O1 keeps them cheap to compile
# while the suite, which runs far more often than an unchanged object is rebuilt, stays fast.
# Line tables (-g1) cost a quarter less to compile than full debug info, and keep the file:line
# frames that the crash containment tests check.
set(test_compile_options -O1 -g1)

set(PGO OFF CACHE STRING "Simulator PGO mode: OFF, TRAIN or USE")
set_property(CACHE PGO PROPERTY STRINGS OFF TRAIN USE)
set(profile_dir "${CMAKE_BINARY_DIR}/pgo")
if(PGO STREQUAL "TRAIN")
    set(profile_flags "-fprofile-generate=${profile_dir}" -fprofile-update=atomic)
    link_libraries(gcov)
elseif(PGO STREQUAL "USE")
    file(GLOB_RECURSE profiles "${profile_dir}/*.gcda")
    if(NOT profiles)
        message(FATAL_ERROR "No simulator PGO data. Build in TRAIN mode and run the training scenarios first.")
    endif()
    set(profile_flags "-fprofile-use=${profile_dir}" -fprofile-correction -fprofile-partial-training)
    # Training scenarios exercise the world entry point and core, not unit tests.
    list(APPEND COMPILATION_FLAGS -Wno-error=missing-profile)
elseif(NOT PGO STREQUAL "OFF")
    message(FATAL_ERROR "PGO must be OFF, TRAIN or USE")
endif()
if(profile_flags)
    list(APPEND profile_flags "-fprofile-prefix-path=${CMAKE_BINARY_DIR}")
endif()

# Opt in to the checked build while chasing a bug, rather than paying for it on every run.
option(ENABLE_DEBUG_CONTAINERS "Enable libstdc++ debug containers" OFF)
if(ENABLE_DEBUG_CONTAINERS)
    list(APPEND COMPILATION_FLAGS -D_GLIBCXX_DEBUG)
endif()

option(ENABLE_SANITIZERS "Enable the address and undefined behaviour sanitizers" OFF)
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# === Libraries ===

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE
)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
# The tests provide their own main().
set_property(TARGET gtest_main PROPERTY EXCLUDE_FROM_ALL ON)
target_compile_options(gtest PRIVATE ${test_compile_options})

FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(tomlplusplus)

# === Options ===

option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)

if(ENABLE_COVERAGE)
    add_compile_options(-O0 -g --coverage)
    add_link_options(--coverage)
endif()



# === Quercus SDK ===

# The simulation is built against the same headers as the Core 1 applications, so that the
# RPC wire contract cannot drift between the two sides. QUERCUS_SDK_ROOT may point at an SDK
# checkout elsewhere; by default the pinned submodule is used.
set(QUERCUS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/q_sdk"
    CACHE PATH "Path to a q_sdk checkout")
# Build directories from before the SDK moved to the repository root still cache its old path.
if(QUERCUS_SDK_ROOT STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/external/q_sdk")
    set(QUERCUS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/q_sdk" CACHE PATH "Path to a q_sdk checkout" FORCE)
endif()

if(NOT EXISTS "${QUERCUS_SDK_ROOT}/include/quercus/rpc_wire.h")
    message(FATAL_ERROR
        "Quercus SDK headers not found at ${QUERCUS_SDK_ROOT}.\n"
        "Run: git submodule update --init --recursive")
endif()

# === Builds ===

include(cmake/libbacktrace.cmake)

add_library(core STATIC
    src/firmware/inprocess_backend.cpp
    src/firmware/crash_posix.cpp
    src/firmware/crash_report.cpp
    src/firmware/crash_watchdog.cpp
    ${backtrace_sources}
    src/sim/module_clock.cpp
    src/sim/runtime.cpp
    src/sim/pacing.cpp
    src/platform/platform.cpp
    src/platform/rpc_worker_pool.cpp
    src/rpc/frame.cpp
    src/seed/seed.cpp
    src/engine/task_queue.cpp
    src/world/world.cpp
    src/world/calibration.cpp
    src/net/network.cpp
    src/trace/trace.cpp
    src/util.cpp 
    src/settings.cpp
    src/event/events.cpp
    src/ltl/lexer.cpp
    src/ltl/parser.cpp
    src/ltl/checker.cpp
    src/ltl/monitor.cpp
    src/ltl/observation.cpp
)

add_library(core_interface INTERFACE)
target_link_libraries(core_interface INTERFACE
    pthread
    tomlplusplus::tomlplusplus
)
target_include_directories(core_interface INTERFACE "${QUERCUS_SDK_ROOT}/include")
target_link_libraries(core PUBLIC core_interface)

target_include_directories(core PRIVATE "${libbacktrace_SOURCE_DIR}")
target_compile_options(core PRIVATE ${COMPILATION_FLAGS} ${sim_lto} ${profile_flags})
set_source_files_properties(src/firmware/crash_posix.cpp PROPERTIES
    COMPILE_OPTIONS -fno-omit-frame-pointer)

# Prelink the core into one relocatable object, so that its LTO work only runs once. Worlds and
# the tests link this object.
if(QUERCUS_COSMOPOLITAN)
    set(prelink_cxx "${COSMOCC_TOOLCHAIN_PATH}/bin/${CMAKE_SYSTEM_PROCESSOR}-linux-cosmo-g++")
    # GCC can't prove libc++'s impossible std::string growth path unreachable, and warns about it.
    set(prelink_warnings -Wno-alloc-size-larger-than)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(cosmocc_abi -ffixed-x18 -ffixed-x28 -fsigned-char)
    else()
        set(cosmocc_abi -mno-red-zone -mno-tls-direct-seg-refs)
    endif()
    set(world_cxx "${COSMOCC_TOOLCHAIN_PATH}/bin/${CMAKE_SYSTEM_PROCESSOR}-unknown-cosmo-c++")
    set(world_objcopy "${COSMOCC_TOOLCHAIN_PATH}/bin/${CMAKE_SYSTEM_PROCESSOR}-linux-cosmo-objcopy")
    set(world_definitions _COSMO_SOURCE)
    set(world_toolchain cosmocc)
else()
    set(prelink_cxx "${CMAKE_CXX_COMPILER}")
    if(prelink_lto)
        # Released GCC (up to at least 16.2) crashes in lto-partition.cc when it partitions a
        # relocatable link. Only the project's cosmocc carries the fix.
        set(prelink_partition -flto-partition=none)
    endif()
    set(world_cxx "${CMAKE_CXX_COMPILER}")
    set(world_objcopy "${CMAKE_OBJCOPY}")
    set(world_toolchain host)
endif()
set(sim_core_object "${CMAKE_BINARY_DIR}/qsim-core.o")
add_custom_command(OUTPUT "${sim_core_object}"
    COMMAND "${prelink_cxx}" -nostdlib -r -O3 ${prelink_lto} ${prelink_partition} ${prelink_warnings}
        ${profile_flags} ${cosmocc_abi} -o "${sim_core_object}" -Wl,--whole-archive $<TARGET_FILE:core> -Wl,--no-whole-archive
    DEPENDS core
    COMMAND_EXPAND_LISTS VERBATIM)
add_custom_target(qsim-core DEPENDS "${sim_core_object}")

# The world entry point. qsim links it with a layout and its firmware objects into a world.
set(QSIM_SDL3_INCLUDE_DIR "" CACHE PATH "SDL3 include directory (optional checkout for offline builds)")
if(NOT QSIM_SDL3_INCLUDE_DIR)
    FetchContent_Declare(sdl3_headers
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
        URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR quercus-headers-only)
    FetchContent_MakeAvailable(sdl3_headers)
    set(QSIM_SDL3_INCLUDE_DIR "${sdl3_headers_SOURCE_DIR}/include")
endif()
# Cosmopolitan worlds embed SDL3 from here. Host-compiler worlds load the system's SDL3.
set(QSIM_SDL3_ASSETS "" CACHE PATH "Directory of prebuilt SDL3 artifacts for this platform")
add_library(world_entry OBJECT
    src/scenario/main.cpp src/firmware/linked_world.cpp
    src/visualization/buttons.cpp src/visualization/sdl.cpp src/visualization/visualization.cpp)
target_link_libraries(world_entry PRIVATE core_interface)
target_include_directories(world_entry SYSTEM PRIVATE "${QSIM_SDL3_INCLUDE_DIR}")
target_compile_options(world_entry PRIVATE ${COMPILATION_FLAGS} ${profile_flags})
target_compile_definitions(world_entry PRIVATE APP_VERSION="${PROJECT_VERSION}" ${world_definitions})

add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/qsim.o"
    COMMAND "${prelink_cxx}" -nostdlib -r -o "${CMAKE_BINARY_DIR}/qsim.o"
        $<TARGET_OBJECTS:world_entry> "${sim_core_object}"
    DEPENDS world_entry $<TARGET_OBJECTS:world_entry> "${sim_core_object}"
    COMMAND_EXPAND_LISTS VERBATIM)
add_custom_target(qsim-world ALL DEPENDS "${CMAKE_BINARY_DIR}/qsim.o")
configure_file(src/firmware/qsim.py qsim @ONLY)

# === Formatting ===
file(GLOB_RECURSE FORMAT_FILES
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h"

    "${CMAKE_SOURCE_DIR}/test/*.c"
    "${CMAKE_SOURCE_DIR}/test/*.cpp"
    "${CMAKE_SOURCE_DIR}/test/*.h"
)

# Leave out what qbuild generates while it builds the test applets.
list(FILTER FORMAT_FILES EXCLUDE REGEX "/test/applets/(build_[^/]*|out)/")

add_custom_target(format
    COMMAND clang-format-22 -style=file -i ${FORMAT_FILES}
    VERBATIM)

add_custom_target(format-check
    COMMAND clang-format-22 -style=file --dry-run --Werror ${FORMAT_FILES}
    VERBATIM)

# === Subdirectories ===

include(CTest)
enable_testing()
add_subdirectory(test)
