# Build with the cosmocc bundle whose CMake runs this configure. Its compilers, CMake, Ninja, Python
# and shell are all Cosmopolitan programs, which start one another directly on every system.
get_filename_component(cosmocc_bin "${CMAKE_COMMAND}" DIRECTORY)
get_filename_component(COSMOCC_TOOLCHAIN_PATH "${cosmocc_bin}" DIRECTORY)
if(NOT EXISTS "${cosmocc_bin}/cosmocc-env.exe" OR NOT EXISTS "${cosmocc_bin}/python")
    message(FATAL_ERROR "cosmocc builds run under the cosmocc bundle's own CMake, not ${CMAKE_COMMAND}. "
        "Activate the bundle (see README.md) and configure again, "
        "or configure with -DQSIM_COSMOCC=OFF for a host-compiler build.")
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
# Downloads verify against the CA bundle embedded in the bundle's CMake. Release 2026.11.17-46
# builds its CMake with a CA path from the build machine, so name the embedded one until a release
# fixes that: on Linux, CMake found the system's bundle first, but on macOS and Windows every
# download failed.
if(NOT DEFINED CMAKE_TLS_CAINFO AND NOT DEFINED ENV{SSL_CERT_FILE})
    set(CMAKE_TLS_CAINFO /zip/usr/share/ssl/cert.pem)
endif()
# The bundle's Python, which starts the bundle's programs, runs the build's scripts and tests.
set(Python3_EXECUTABLE "${cosmocc_bin}/python" CACHE FILEPATH "The cosmocc bundle's Python")
set(QUERCUS_COSMOPOLITAN ON)
