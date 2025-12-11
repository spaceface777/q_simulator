cmake_minimum_required(VERSION 3.10.0)
project(quercus-router-shadow VERSION 0.1.0 LANGUAGES C CXX)

add_executable(server src/server.cpp)
add_executable(client src/client.cpp)

include(CTest)
enable_testing()

