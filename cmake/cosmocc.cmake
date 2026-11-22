# Build with the cosmocc bundle whose CMake runs this configure. Its compilers, CMake, Ninja, Python
# and shell are all Cosmopolitan programs, which start one another directly on every system.
get_filename_component(cosmocc_bin "${CMAKE_COMMAND}" DIRECTORY)
get_filename_component(COSMOCC_TOOLCHAIN_PATH "${cosmocc_bin}" DIRECTORY)

# Cosmopolitan supports Windows on x86-64 only, also on Arm, where it runs emulated.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(CMAKE_SYSTEM_PROCESSOR x86_64)
else()
    set(CMAKE_SYSTEM_PROCESSOR aarch64)
endif()
set(CMAKE_SYSTEM_NAME Linux)
# CMake finds the binary tools by the compilers' prefix.
set(CMAKE_C_COMPILER "${cosmocc_bin}/${CMAKE_SYSTEM_PROCESSOR}-unknown-cosmo-cc")
set(CMAKE_CXX_COMPILER "${cosmocc_bin}/${CMAKE_SYSTEM_PROCESSOR}-unknown-cosmo-c++")
# The bundle's Python, which starts the bundle's programs, runs the build's scripts and tests.
set(Python3_EXECUTABLE "${cosmocc_bin}/python")
set(QUERCUS_COSMOPOLITAN ON)
