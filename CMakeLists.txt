cmake_minimum_required(VERSION 3.12.0)
project(simulation VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_CXX_STANDARD 23)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(COMPILATION_FLAGS -Wall -Wextra -Werror)
if(QUERCUS_COSMOPOLITAN)
    # GCC 16 emits false positives in Cosmopolitan's libc++ string implementation.
    list(APPEND COMPILATION_FLAGS -Wno-error=array-bounds -Wno-error=stringop-overflow -Wno-error=restrict)
    if(CMAKE_HOST_APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        # The AArch64 driver emits ELF. Wrap each executable for the Apple Silicon loader.
        set(cosmocc_bin "${COSMOCC_TOOLCHAIN_PATH}/bin")
        string(APPEND CMAKE_CXX_LINK_EXECUTABLE
            " && \"${CMAKE_COMMAND}\" -E rename <TARGET> <TARGET>.dbg && sh \"${cosmocc_bin}/apelink\" -l \"${cosmocc_bin}/ape-aarch64.elf\" -M \"${cosmocc_bin}/ape-m1.c\" -o <TARGET> <TARGET>.dbg")
    endif()
endif()
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -flto=auto -g")

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
FetchContent_MakeAvailable(googletest)

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
set(QUERCUS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/q_sdk"
    CACHE PATH "Path to a q_sdk checkout")

if(NOT EXISTS "${QUERCUS_SDK_ROOT}/include/quercus/rpc_wire.h")
    message(FATAL_ERROR
        "Quercus SDK headers not found at ${QUERCUS_SDK_ROOT}.\n"
        "Run: git submodule update --init --recursive")
endif()

# === Builds ===

add_library(core STATIC
    src/firmware/inprocess_backend.cpp
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

target_link_libraries(core PUBLIC
    pthread
    tomlplusplus::tomlplusplus
)

target_include_directories(core PUBLIC "${QUERCUS_SDK_ROOT}/include")

target_compile_options(core PRIVATE ${COMPILATION_FLAGS})

if(QUERCUS_COSMOPOLITAN)
    add_library(world_entry OBJECT src/scenario/main.cpp src/firmware/linked_world.cpp)
    target_link_libraries(world_entry PRIVATE core)
    target_compile_options(world_entry PRIVATE ${COMPILATION_FLAGS})
    target_compile_definitions(world_entry PRIVATE APP_VERSION="${PROJECT_VERSION}")

    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(cosmocc_abi -ffixed-x18 -ffixed-x28 -fsigned-char)
    else()
        set(cosmocc_abi -mno-red-zone -mno-tls-direct-seg-refs)
    endif()
    # GCC 16 can fail during partitioning for this relocatable LTO link.
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/qsim.o"
        COMMAND "${COSMOCC_TOOLCHAIN_PATH}/bin/${CMAKE_SYSTEM_PROCESSOR}-linux-cosmo-g++"
            -nostdlib -r -O3 -flto=auto -flto-partition=one -flinker-output=nolto-rel ${cosmocc_abi}
            -o "${CMAKE_BINARY_DIR}/qsim.o" $<TARGET_OBJECTS:world_entry>
            -Wl,--whole-archive $<TARGET_FILE:core> -Wl,--no-whole-archive
        DEPENDS world_entry $<TARGET_OBJECTS:world_entry> core
        COMMAND_EXPAND_LISTS VERBATIM)
    add_custom_target(qsim-world ALL DEPENDS "${CMAKE_BINARY_DIR}/qsim.o")
    configure_file(src/firmware/qsim.py qsim @ONLY)
endif()

# === Formatting ===
file(GLOB_RECURSE FORMAT_FILES
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h"

    "${CMAKE_SOURCE_DIR}/test/*.c"
    "${CMAKE_SOURCE_DIR}/test/*.cpp"
    "${CMAKE_SOURCE_DIR}/test/*.h"
)

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
