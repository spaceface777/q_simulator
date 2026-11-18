# Build with the cosmocc bundle whose CMake runs this configure. Its compilers, CMake, Ninja, Python
# and shell are all Cosmopolitan programs, which start one another directly on every system.
get_filename_component(cosmocc_bin "${CMAKE_COMMAND}" DIRECTORY)
get_filename_component(COSMOCC_TOOLCHAIN_PATH "${cosmocc_bin}" DIRECTORY)
# Each release from 2026.11.18-54 on names itself in the first line of its BUILD_INFO:
# "# cosmocc <date>-<number>". The build needs release 64 or later, whose connect() works on
# Windows: earlier releases bound the socket to an address of stack garbage first.
set(cosmocc_min_release 64)
set(cosmocc_build_info "${COSMOCC_TOOLCHAIN_PATH}/BUILD_INFO")
if(NOT EXISTS "${cosmocc_build_info}")
    message(FATAL_ERROR "cosmocc builds run under the CMake of a cosmocc bundle, release 2026.11.18-64 or later, "
        "not ${CMAKE_COMMAND}, which has no ${cosmocc_build_info}. "
        "Install a current release, activate it (see README.md) and configure again, "
        "or configure with -DQSIM_COSMOCC=OFF for a host-compiler build.")
endif()
file(STRINGS "${cosmocc_build_info}" cosmocc_release LIMIT_COUNT 1)
if(NOT cosmocc_release MATCHES "^# cosmocc [0-9.]+-([0-9]+)$")
    message(FATAL_ERROR "${cosmocc_build_info} does not start with \"# cosmocc <date>-<number>\".")
endif()
if(CMAKE_MATCH_1 LESS cosmocc_min_release)
    string(REPLACE "# cosmocc " "" cosmocc_release "${cosmocc_release}")
    message(FATAL_ERROR "The cosmocc bundle in ${COSMOCC_TOOLCHAIN_PATH} is release ${cosmocc_release}; "
        "the build needs release ${cosmocc_min_release} or later. Install a current release (see README.md).")
endif()

# Cosmopolitan supports Windows on x86-64 only, also on Arm, where it runs emulated.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
    set(cosmocc_arch x86_64)
else()
    set(cosmocc_arch aarch64)
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR ${cosmocc_arch})
set(CMAKE_C_COMPILER "${cosmocc_bin}/${cosmocc_arch}-unknown-cosmo-cc")
set(CMAKE_CXX_COMPILER "${cosmocc_bin}/${cosmocc_arch}-unknown-cosmo-c++")
set(CMAKE_AR "${cosmocc_bin}/${cosmocc_arch}-linux-cosmo-gcc-ar" CACHE FILEPATH "Cosmopolitan archiver")
set(CMAKE_RANLIB "${cosmocc_bin}/${cosmocc_arch}-linux-cosmo-gcc-ranlib" CACHE FILEPATH "Cosmopolitan archive indexer")
# The bundle's Python, which starts the bundle's programs, runs the build's scripts and tests.
set(Python3_EXECUTABLE "${cosmocc_bin}/python" CACHE FILEPATH "The cosmocc bundle's Python")
set(QUERCUS_COSMOPOLITAN ON)
