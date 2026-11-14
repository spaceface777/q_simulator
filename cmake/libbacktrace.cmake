# Cosmopolitan worlds use one fixed-address ELF symbol file. The Linux developer
# build can use the system's loaded ELF objects, including PIE executables.
FetchContent_Declare(
    libbacktrace
    GIT_REPOSITORY https://github.com/ianlancetaylor/libbacktrace.git
    GIT_TAG        ab3c042b8033efd5d396310f71cb3bc62885062b
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

set(backtrace_sources alloc.c atomic.c dwarf.c elf.c fileline.c posix.c read.c sort.c state.c)
list(TRANSFORM backtrace_sources PREPEND "${libbacktrace_SOURCE_DIR}/")
set_source_files_properties(${backtrace_sources} PROPERTIES COMPILE_OPTIONS -w
    INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/libbacktrace;${libbacktrace_SOURCE_DIR}"
    COMPILE_DEFINITIONS _GNU_SOURCE)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/libbacktrace/backtrace-supported.h" CONTENT
    "#define BACKTRACE_SUPPORTED 1\n#define BACKTRACE_USES_MALLOC 1\n#define BACKTRACE_SUPPORTS_THREADS 1\n#define BACKTRACE_SUPPORTS_DATA 1\n")
