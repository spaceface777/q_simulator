# One download as the simulator's configure does it: cmake starts ninja for FetchContent's
# sub-build, and ninja starts `cmake -P` for the download, from github.com and, after its
# redirect, raw.githubusercontent.com.
cmake_minimum_required(VERSION 3.24)
project(chain NONE)
include(FetchContent)
FetchContent_Declare(version
  URL https://github.com/zephyrproject-rtos/zephyr/raw/v4.5.0/VERSION
  DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(version)
