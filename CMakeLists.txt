cmake_minimum_required(VERSION 3.10.0)
project(simulation VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Debug)

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# === Libraries ===

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
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

add_executable(simulation 
    src/main.cpp 
    src/tcp/server.cpp 
    src/tcp/client.cpp 
    src/util.cpp 
    src/module/module.cpp
    src/message/message.cpp
    src/tub/tub.cpp
    src/visualization/visualization.cpp
    src/visualization/buttons.cpp
    src/settings.cpp
    src/event/events.cpp)

target_compile_definitions(simulation PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    APP_OS="${CMAKE_SYSTEM_NAME}"
    APP_ARCH="${CMAKE_SYSTEM_PROCESSOR}"
)

target_link_libraries(simulation pthread 
    tomlplusplus::tomlplusplus
    SDL2::SDL2
)

configure_file(${CMAKE_SOURCE_DIR}/events.toml ${CMAKE_BINARY_DIR}/events.toml COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/events_example.toml ${CMAKE_BINARY_DIR}/events_example.toml COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/settings.toml ${CMAKE_BINARY_DIR}/settings.toml COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/settings_example.toml ${CMAKE_BINARY_DIR}/settings_example.toml COPYONLY)

# === Testing ===

include(CTest)
enable_testing()
add_subdirectory(test)
