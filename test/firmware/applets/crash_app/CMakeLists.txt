# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

if(NOT DEFINED QUERCUS_SDK_ROOT)
	set(QUERCUS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../external/q_sdk")
endif()
include("${QUERCUS_SDK_ROOT}/cmake/quercus_app.cmake")
quercus_configure_application()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app)

quercus_add_application_runtime(app)
target_sources(app PRIVATE src/main.c)
