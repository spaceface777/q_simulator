cmake_minimum_required(VERSION 3.20)

# Build with cosmocc unless the configure names another toolchain file or asks for the host compiler.
option(QSIM_COSMOCC "Build with the cosmocc toolchain (OFF: the host's clang and lld, Linux only)" ON)
if(QSIM_COSMOCC AND NOT DEFINED CMAKE_TOOLCHAIN_FILE AND NOT DEFINED ENV{CMAKE_TOOLCHAIN_FILE})
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cosmocc.cmake" CACHE FILEPATH "Toolchain file")
endif()

project(simulation VERSION 1.0.0 LANGUAGES C CXX)
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "The simulator builds with clang and lld: configure with CC=clang CXX=clang++.")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_CXX_STANDARD 23)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(COMPILATION_FLAGS -Wall -Wextra -Werror)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -g")
# Every link, the prelinks' and the worlds' (qsim.py) too.
set(link_options -fuse-ld=lld)
if(QUERCUS_COSMOPOLITAN AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    # The AArch64 driver links ELF, and keeps it in *.exe.dbg. Make each executable an Actually
    # Portable Executable, which also runs on macOS. A list item is one more command.
    set(cosmocc_bin "${COSMOCC_TOOLCHAIN_PATH}/bin")
    list(APPEND CMAKE_CXX_LINK_EXECUTABLE
        "\"${cosmocc_bin}/apelink\" -l \"${cosmocc_bin}/ape-aarch64.elf\" -M \"${cosmocc_bin}/ape-m1.c\" -o <TARGET> <TARGET>.dbg")
endif()

# Only the core carries LTO bitcode. The qsim-core.o prelink below generates its code once, and
# both the world entry point and the tests link it.
option(ENABLE_LTO "Enable LTO in optimized builds" ON)
if(ENABLE_LTO)
    set(sim_lto $<$<CONFIG:Release>:-flto=thin>)
    set(prelink_lto -flto=thin)
endif()

# Test code and GoogleTest skip LTO and the expensive -O3 passes. -O1 keeps them cheap to compile
# while the suite, which runs far more often than an unchanged object is rebuilt, stays fast.
# Line tables (-g1) cost a quarter less to compile than full debug info, and keep the file:line
# frames that the crash containment tests check.
set(test_compile_options -O1 -g1)

# A world writes one profile, of the simulator and its firmware: see docs/OPTIMIZATION.md.
set(PGO OFF CACHE STRING "Simulator PGO mode: OFF, TRAIN or USE")
set_property(CACHE PGO PROPERTY STRINGS OFF TRAIN USE)
set(profile_dir "${CMAKE_BINARY_DIR}/pgo")
if(PGO STREQUAL "TRAIN")
    set(profile_flags "-fprofile-generate=${profile_dir}" -fprofile-update=atomic)
    list(APPEND link_options -fprofile-generate)
elseif(PGO STREQUAL "USE")
    file(GLOB profiles "${profile_dir}/*.profraw")
    if(NOT profiles)
        message(FATAL_ERROR "No simulator profile in ${profile_dir}. Build in TRAIN mode and run the training scenarios first.")
    endif()
    get_filename_component(compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(LLVM_PROFDATA llvm-profdata HINTS "${compiler_dir}" REQUIRED)
    execute_process(COMMAND "${LLVM_PROFDATA}" merge -o "${profile_dir}/default.profdata" ${profiles}
        COMMAND_ERROR_IS_FATAL ANY)
    set(profile_flags "-fprofile-use=${profile_dir}")
elseif(NOT PGO STREQUAL "OFF")
    message(FATAL_ERROR "PGO must be OFF, TRAIN or USE")
endif()

# Opt in to the checked build while chasing a bug, rather than paying for it on every run.
option(ENABLE_DEBUG_CONTAINERS "Enable the C++ library's debug containers" OFF)
if(ENABLE_DEBUG_CONTAINERS)
    list(APPEND COMPILATION_FLAGS -D_GLIBCXX_DEBUG -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG)
endif()

# Every finding stops the program, so that tests fail. Cosmopolitan Libc has no address sanitizer.
option(ENABLE_SANITIZERS "Enable the address (host builds) and undefined behaviour sanitizers" OFF)
if(ENABLE_SANITIZERS)
    set(instrument_flags -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    if(NOT QUERCUS_COSMOPOLITAN)
        list(APPEND instrument_flags -fsanitize=address)
    endif()
endif()

# Source-based coverage: each process writes its own profile to coverage/ (see README.md).
option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
if(ENABLE_COVERAGE)
    list(APPEND instrument_flags -O0 "-fprofile-instr-generate=${CMAKE_BINARY_DIR}/coverage/%m-%p.profraw"
        -fcoverage-mapping)
endif()
add_compile_options(${instrument_flags})
list(APPEND link_options ${instrument_flags})
add_link_options(${link_options})
# qsim.py links worlds as CMake links executables.
separate_arguments(world_link_options NATIVE_COMMAND "${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}")
list(APPEND world_link_options ${link_options})

# === Libraries ===

include(FetchContent)

# Release archives rather than Git: the build then needs no git, which a minimal PATH lacks.
FetchContent_Declare(
    googletest
    URL      https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
# The tests provide their own main().
set_property(TARGET gtest_main PROPERTY EXCLUDE_FROM_ALL ON)
target_compile_options(gtest PRIVATE ${test_compile_options})

FetchContent_Declare(
    tomlplusplus
    URL      https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(tomlplusplus)

# === Quercus SDK ===

# The simulation is built against the same headers as the Core 1 applications, so that the
# RPC wire contract cannot drift between the two sides. QUERCUS_SDK_ROOT may point at an SDK
# checkout elsewhere; by default the pinned submodule is used.
set(QUERCUS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/q_sdk"
    CACHE PATH "Path to a q_sdk checkout")

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
set(sim_core_object "${CMAKE_BINARY_DIR}/qsim-core.o")
add_custom_command(OUTPUT "${sim_core_object}"
    COMMAND "${CMAKE_CXX_COMPILER}" -nostdlib -r -O3 -fuse-ld=lld ${prelink_lto} -o "${sim_core_object}"
        -Wl,--whole-archive $<TARGET_FILE:core> -Wl,--no-whole-archive
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
target_compile_definitions(world_entry PRIVATE APP_VERSION="${PROJECT_VERSION}" _COSMO_SOURCE)

add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/qsim.o"
    COMMAND "${CMAKE_CXX_COMPILER}" -nostdlib -r -fuse-ld=lld -o "${CMAKE_BINARY_DIR}/qsim.o"
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

# pip installs clang-format 22 under its plain name.
find_program(CLANG_FORMAT NAMES clang-format-22 clang-format)
add_custom_target(format
    COMMAND "${CLANG_FORMAT}" -style=file -i ${FORMAT_FILES}
    VERBATIM)

add_custom_target(format-check
    COMMAND "${CLANG_FORMAT}" -style=file --dry-run --Werror ${FORMAT_FILES}
    VERBATIM)

# === Subdirectories ===

include(CTest)
enable_testing()
add_subdirectory(test)
