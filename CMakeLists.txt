cmake_minimum_required(VERSION 3.10.0)
project(quercus-router-shadow VERSION 0.1.0 LANGUAGES C CXX)

# === Builds ===

add_executable(main 
    src/main.cpp 
    src/events/parser.cpp 
    src/tcp/tcp.cpp 
    src/tcp/client.cpp 
    src/util.cpp 
    src/module/module.cpp
    src/packet/packet.cpp)
target_link_libraries(main pthread)

configure_file(${CMAKE_SOURCE_DIR}/events.toml ${CMAKE_BINARY_DIR}/events.toml COPYONLY)

# === Testing ===

include(CTest)
enable_testing()

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        release-1.11.0
)
FetchContent_MakeAvailable(googletest)

add_subdirectory(test)

