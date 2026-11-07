cmake_minimum_required(VERSION 3.20)
project(toolchain_selftest LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4 OR NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "Expected 32-bit Clang, got ${CMAKE_C_COMPILER_ID} "
                      "with ${CMAKE_SIZEOF_VOID_P}-byte pointers")
endif()

add_library(greeting STATIC ../greeting.c)
add_executable(selftest ../hello.cpp)
target_link_libraries(selftest PRIVATE greeting)
# A static C++ runtime lets the program run without the toolchain's DLLs on PATH.
target_link_options(selftest PRIVATE -static)
