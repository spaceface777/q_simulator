# Use the Cosmopolitan driver for the host CPU.
set(COSMOCC_TOOLCHAIN_PATH "$ENV{COSMOCC_TOOLCHAIN_PATH}" CACHE PATH "Extracted cosmocc toolchain")
if(NOT COSMOCC_TOOLCHAIN_PATH)
    # Fall back to a cosmocc on PATH or in the conventional install location.
    find_program(cosmocc_driver NAMES cosmocc HINTS /opt/cosmocc/bin NO_CACHE)
    if(cosmocc_driver)
        get_filename_component(cosmocc_driver "${cosmocc_driver}" REALPATH)
        get_filename_component(cosmocc_driver_dir "${cosmocc_driver}" DIRECTORY)
        get_filename_component(found_toolchain "${cosmocc_driver_dir}" DIRECTORY)
        set(COSMOCC_TOOLCHAIN_PATH "${found_toolchain}" CACHE PATH "Extracted cosmocc toolchain" FORCE)
    endif()
endif()
if(NOT COSMOCC_TOOLCHAIN_PATH)
    message(FATAL_ERROR "Set COSMOCC_TOOLCHAIN_PATH to the extracted cosmocc toolchain, "
        "or configure with -DQSIM_COSMOCC=OFF for a host-compiler build")
endif()

# The uname() system call, not CMAKE_HOST_SYSTEM_PROCESSOR: CMake finds that by running uname, which
# macOS cannot start when the toolchain's own uname, an APE file, comes first on PATH.
cmake_host_system_information(RESULT cosmocc_host_cpu QUERY OS_PLATFORM)
if(CMAKE_HOST_WIN32 OR cosmocc_host_cpu MATCHES "^(x86_64|AMD64|amd64)$")
    set(cosmocc_arch x86_64)
else()
    set(cosmocc_arch aarch64)
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR ${cosmocc_arch})
set(cosmocc_bin "${COSMOCC_TOOLCHAIN_PATH}/bin")
if(CMAKE_HOST_WIN32)
    # Windows starts only executables, and the compiler drivers are shell scripts: start them with
    # the toolchain's sh (dash, in its POSIX userland), or with its bash in releases without one.
    # Its binary tools also exist under *.exe names.
    set(cosmocc_sh "${COSMOCC_TOOLCHAIN_PATH}/posix/bin/sh.exe")
    if(NOT EXISTS "${cosmocc_sh}")
        set(cosmocc_sh "${cosmocc_bin}/bash.exe")
    endif()
    set(CMAKE_C_COMPILER "${cosmocc_sh}" "${cosmocc_bin}/${cosmocc_arch}-unknown-cosmo-cc")
    set(CMAKE_CXX_COMPILER "${cosmocc_sh}" "${cosmocc_bin}/${cosmocc_arch}-unknown-cosmo-c++")
    set(cosmocc_exe .exe)
else()
    set(CMAKE_C_COMPILER "${cosmocc_bin}/${cosmocc_arch}-unknown-cosmo-cc")
    set(CMAKE_CXX_COMPILER "${cosmocc_bin}/${cosmocc_arch}-unknown-cosmo-c++")
    set(cosmocc_exe "")
endif()
set(CMAKE_AR "${cosmocc_bin}/${cosmocc_arch}-linux-cosmo-gcc-ar${cosmocc_exe}" CACHE FILEPATH "Cosmopolitan archiver")
set(CMAKE_RANLIB "${cosmocc_bin}/${cosmocc_arch}-linux-cosmo-gcc-ranlib${cosmocc_exe}" CACHE FILEPATH "Cosmopolitan archive indexer")
if(NOT CMAKE_HOST_WIN32)
    # The archiver tools are APE binaries without a shebang. Only a shell can start them, and the
    # Makefile generator runs archive commands without one, so start them through sh.
    foreach(lang C CXX)
        set(CMAKE_${lang}_ARCHIVE_CREATE "sh <CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_${lang}_ARCHIVE_APPEND "sh <CMAKE_AR> q <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_${lang}_ARCHIVE_FINISH "sh <CMAKE_RANLIB> <TARGET>")
    endforeach()
endif()
set(QUERCUS_COSMOPOLITAN ON)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES COSMOCC_TOOLCHAIN_PATH)
