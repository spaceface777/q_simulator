cmake_minimum_required(VERSION 3.10.0)
project(quercus-router-shadow VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_CXX_STANDARD 17)

# === Libraries ===

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        release-1.11.0
)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
  tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

find_package(SDL2 REQUIRED)

# === Builds ===

add_executable(main 
    src/main.cpp 
    src/event/parser.cpp 
    src/tcp/server.cpp 
    src/tcp/client.cpp 
    src/util.cpp 
    src/module/module.cpp
    src/packet/packet.cpp
    src/tub/tub.cpp
    src/visualization/visualization.cpp)

target_link_libraries(main pthread 
    tomlplusplus::tomlplusplus
    SDL2::SDL2
)

configure_file(${CMAKE_SOURCE_DIR}/events.toml ${CMAKE_BINARY_DIR}/events.toml COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/settings.toml ${CMAKE_BINARY_DIR}/settings.toml COPYONLY)

# === Testing ===

include(CTest)
enable_testing()
add_subdirectory(test)
