
# === Libraries ===

set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SDL3)

# === Builds ===

add_executable(simulation 
    main.cpp
    visualization/visualization.cpp
    visualization/buttons.cpp
)

target_compile_options(simulation PRIVATE ${COMPILATION_FLAGS})

target_link_libraries(simulation PRIVATE 
    core 
    SDL3::SDL3
)

target_compile_definitions(simulation PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    APP_OS="${CMAKE_SYSTEM_NAME}"
    APP_ARCH="${CMAKE_SYSTEM_PROCESSOR}"
)

configure_file(${CMAKE_SOURCE_DIR}/events_example.toml ${CMAKE_BINARY_DIR}/events_example.toml COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/layout_example.toml ${CMAKE_BINARY_DIR}/layout_example.toml COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/settings_example.toml ${CMAKE_BINARY_DIR}/settings_example.toml COPYONLY)

if(EXISTS ${CMAKE_SOURCE_DIR}/events.toml)
    configure_file(${CMAKE_SOURCE_DIR}/events.toml ${CMAKE_BINARY_DIR}/events.toml COPYONLY)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/layout.toml)
    configure_file(${CMAKE_SOURCE_DIR}/layout.toml ${CMAKE_BINARY_DIR}/layout.toml COPYONLY)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/settings.toml)
    configure_file(${CMAKE_SOURCE_DIR}/settings.toml ${CMAKE_BINARY_DIR}/settings.toml COPYONLY)
endif()