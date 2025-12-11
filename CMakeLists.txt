cmake_minimum_required(VERSION 3.10.0)
project(quercus-router-shadow VERSION 0.1.0 LANGUAGES C CXX)

add_executable(quercus-router-shadow src/main.cpp)

include(CTest)
enable_testing()

