cmake_minimum_required(VERSION 3.12.0)
project(simulation VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_CXX_STANDARD 23)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(COMPILATION_FLAGS -Wall -Wextra -Werror)
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

option(TESTING_CONSOLE_BUILD "Enable console-only testing build" OFF)

# === Quercus SDK ===

# The simulation is built against the same headers as the Core 1 applications, so that the
# RPC wire contract cannot drift between the two sides. QUERCUS_SDK_ROOT may point at an SDK
# checkout elsewhere; by default the pinned submodule is used.
set(QUERCUS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/quercus_sdk"
    CACHE PATH "Path to a quercus_sdk checkout")

if(NOT EXISTS "${QUERCUS_SDK_ROOT}/include/quercus/rpc_wire.h")
    message(FATAL_ERROR
        "Quercus SDK headers not found at ${QUERCUS_SDK_ROOT}.\n"
        "Run: git submodule update --init --recursive")
endif()

# === Builds ===

add_library(core STATIC
    src/childproc/launcher.cpp
    src/sim/runtime.cpp
    src/sim/pacing.cpp
    src/platform/platform.cpp
    src/platform/rpc_worker_pool.cpp
    src/childsync/mailbox.cpp
    src/childsync/barrier.cpp
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

# The headless scenario runner. Deliberately built here rather than under src/, because that
# directory pulls in SDL3 and this target must not: a scenario run has nothing to draw, and
# needing a window system to check an application's behaviour would put the whole mode out of
# reach of CI and of anyone on a machine without X11.
add_executable(simulation-scenario src/scenario/main.cpp)

target_link_libraries(simulation-scenario PRIVATE core)

target_compile_options(simulation-scenario PRIVATE ${COMPILATION_FLAGS})

target_compile_definitions(simulation-scenario PRIVATE APP_VERSION="${PROJECT_VERSION}")

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
    COMMAND clang-format -style=file -i ${FORMAT_FILES}
    VERBATIM)

add_custom_target(format-check
    COMMAND clang-format -style=file --dry-run --Werror ${FORMAT_FILES}
    VERBATIM)

# === Subdirectories ===

if(NOT TESTING_CONSOLE_BUILD)
    add_subdirectory(src)
endif()

include(CTest)
enable_testing()
add_subdirectory(test)
