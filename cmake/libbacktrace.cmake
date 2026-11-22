# Cosmopolitan worlds use one fixed-address ELF symbol file. The Linux developer
# build can use the system's loaded ELF objects, including PIE executables.
FetchContent_Declare(
    libbacktrace
    # An archive rather than Git, like the other dependencies: the build needs no git.
    URL      https://github.com/ianlancetaylor/libbacktrace/archive/ab3c042b8033efd5d396310f71cb3bc62885062b.tar.gz
    URL_HASH SHA256=e6f8c0c31a9ea40f1e1e2716cb35e53fa5ac6c4baa1a418f7a43eaaf65e8fdf8
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(libbacktrace)

set(backtrace_config "#define HAVE_ATOMIC_FUNCTIONS 1
#define HAVE_SYNC_FUNCTIONS 1
#define HAVE_DECL_STRNLEN 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL 1
#define HAVE_LSTAT 1
#define HAVE_READLINK 1
#define HAVE_SYS_MMAN_H 1
#define HAVE_DECL_GETPAGESIZE 1
#define STDC_HEADERS 1
#define BACKTRACE_ELF_SIZE 64
")
if(NOT QUERCUS_COSMOPOLITAN)
    string(APPEND backtrace_config "#define HAVE_DL_ITERATE_PHDR 1\n#define HAVE_LINK_H 1\n")
endif()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/libbacktrace/config.h" CONTENT "${backtrace_config}")

set(backtrace_sources atomic.c dwarf.c elf.c fileline.c mmap.c mmapio.c posix.c sort.c state.c)
list(TRANSFORM backtrace_sources PREPEND "${libbacktrace_SOURCE_DIR}/")
set_source_files_properties(${backtrace_sources} PROPERTIES COMPILE_OPTIONS -w
    INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/libbacktrace;${libbacktrace_SOURCE_DIR}"
    COMPILE_DEFINITIONS _GNU_SOURCE)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/libbacktrace/backtrace-supported.h" CONTENT
    "#define BACKTRACE_SUPPORTED 1\n#define BACKTRACE_USES_MALLOC 0\n#define BACKTRACE_SUPPORTS_THREADS 1\n#define BACKTRACE_SUPPORTS_DATA 1\n")
