# libbacktrace reads DWARF debug information, so that a crash report can name the function, file
# and line of every frame. Its sources are compiled here, without its autotools build, so that
# the MinGW build needs no POSIX shell.

FetchContent_Declare(
    libbacktrace
    GIT_REPOSITORY https://github.com/ianlancetaylor/libbacktrace.git
    GIT_TAG        ab3c042b8033efd5d396310f71cb3bc62885062b
)
FetchContent_MakeAvailable(libbacktrace)

set(_backtrace_source "${libbacktrace_SOURCE_DIR}")
set(_backtrace_generated "${CMAKE_BINARY_DIR}/libbacktrace")

set(_backtrace_sources atomic.c dwarf.c fileline.c posix.c sort.c state.c)
set(_backtrace_config "#define HAVE_ATOMIC_FUNCTIONS 1
#define HAVE_SYNC_FUNCTIONS 1
#define HAVE_DECL_STRNLEN 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1
#define BACKTRACE_XCOFF_SIZE unused
")

if(WIN32)
    list(APPEND _backtrace_sources pecoff.c)
    string(APPEND _backtrace_config "#define HAVE_WINDOWS_H 1
#define HAVE_TLHELP32_H 1
#define HAVE_DECL__PGMPTR 1
#define HAVE_DECL_GETPAGESIZE 0
#define BACKTRACE_ELF_SIZE unused
")
else()
    string(APPEND _backtrace_config "#define HAVE_DLFCN_H 1
#define HAVE_FCNTL 1
#define HAVE_LSTAT 1
#define HAVE_READLINK 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_MMAN_H 1
#define HAVE_DECL_GETPAGESIZE 1
#define HAVE_DECL__PGMPTR 0
#define HAVE_CLOCK_GETTIME 1
")
    if(APPLE)
        list(APPEND _backtrace_sources macho.c)
        string(APPEND _backtrace_config "#define HAVE_MACH_O_DYLD_H 1
#define BACKTRACE_ELF_SIZE unused
")
    else()
        list(APPEND _backtrace_sources elf.c)
        math(EXPR _backtrace_bits "${CMAKE_SIZEOF_VOID_P} * 8")
        string(APPEND _backtrace_config "#define HAVE_DL_ITERATE_PHDR 1
#define HAVE_LINK_H 1
#define BACKTRACE_ELF_SIZE ${_backtrace_bits}
")
    endif()
endif()

file(GENERATE OUTPUT "${_backtrace_generated}/config.h" CONTENT "${_backtrace_config}")
file(GENERATE OUTPUT "${_backtrace_generated}/backtrace-supported.h" CONTENT "#define BACKTRACE_SUPPORTED 1
#define BACKTRACE_USES_MALLOC 1
#define BACKTRACE_SUPPORTS_THREADS 1
#define BACKTRACE_SUPPORTS_DATA 1
")

list(TRANSFORM _backtrace_sources PREPEND "${_backtrace_source}/")

# Line tables name directories relative to the compilation directory. libbacktrace joins only a
# file and its directory, so a build that records relative paths (ccache with base_dir does)
# would print paths relative to an unknown directory. Compile a copy of dwarf.c that also joins
# the compilation directory, for DWARF 4 and for DWARF 5 line tables.
file(READ "${_backtrace_source}/dwarf.c" _backtrace_dwarf)
function(_backtrace_insert_after anchor text)
    string(FIND "${_backtrace_dwarf}" "${anchor}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "libbacktrace dwarf.c changed; update the directory patch in ${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    endif()
    string(REPLACE "${anchor}" "${anchor}${text}" _patched "${_backtrace_dwarf}")
    set(_backtrace_dwarf "${_patched}" PARENT_SCOPE)
endfunction()
set(_backtrace_join "
		{
		  size_t base_len = strlen (BASE);
		  size_t dir_len = strlen (DIR);
		  char *joined = (char *) backtrace_alloc (state, base_len + dir_len + 2,
							   hdr_buf->error_callback,
							   hdr_buf->data);
		  if (joined == NULL)
		    return 0;
		  memcpy (joined, BASE, base_len);
		  joined[base_len] = '/';
		  memcpy (joined + base_len + 1, DIR, dir_len + 1);
		  DIR = joined;
		}
")
# DWARF 4: the directory table follows the compilation directory, which is entry 0.
string(REPLACE "BASE" "u->comp_dir" _join4 "${_backtrace_join}")
string(REPLACE "DIR" "hdr->dirs[i]" _join4 "${_join4}")
_backtrace_insert_after("      hdr->dirs[i] = read_string (hdr_buf);
      if (hdr->dirs[i] == NULL)
	return 0;
" "      if (u->comp_dir != NULL && !IS_ABSOLUTE_PATH (hdr->dirs[i]))${_join4}")
# DWARF 5: directory entry 0 is the compilation directory.
string(REPLACE "BASE" "hdr->dirs[0]" _join5 "${_backtrace_join}")
string(REPLACE "DIR" "dir" _join5 "${_join5}")
_backtrace_insert_after("	      dir = hdr->dirs[val.u.uint];
" "	      if (dir != NULL && val.u.uint > 0 && !IS_ABSOLUTE_PATH (dir)
		  && hdr->dirs[0] != NULL)${_join5}")
file(GENERATE OUTPUT "${_backtrace_generated}/dwarf.c" CONTENT "${_backtrace_dwarf}")
list(REMOVE_ITEM _backtrace_sources "${_backtrace_source}/dwarf.c")
list(APPEND _backtrace_sources "${_backtrace_generated}/dwarf.c")

# The Mach-O symbol table also holds absolute symbols, which are values rather than addresses:
# Zephyr emits Kconfig values and sizes such as CONFIG_LOG_FLUSH_SLEEP_US = 10000. libbacktrace
# slides them like code addresses, so a frame just above one was named after it. Compile a copy of
# macho.c that keeps only symbols defined in a section, as the ELF and PE readers already do.
if(APPLE)
    file(READ "${_backtrace_source}/macho.c" _backtrace_macho)
    set(_absolute "    case MACH_O_N_ABS:\n      return 1;\n")
    string(FIND "${_backtrace_macho}" "${_absolute}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "libbacktrace macho.c changed; update the absolute-symbol patch in ${CMAKE_CURRENT_LIST_FILE}")
    endif()
    string(REPLACE "${_absolute}" "    case MACH_O_N_ABS:\n      return 0;\n" _backtrace_macho "${_backtrace_macho}")
    file(GENERATE OUTPUT "${_backtrace_generated}/macho.c" CONTENT "${_backtrace_macho}")
    list(REMOVE_ITEM _backtrace_sources "${_backtrace_source}/macho.c")
    list(APPEND _backtrace_sources "${_backtrace_generated}/macho.c")
endif()

# A Windows state also adds each DLL loaded later, from a loader callback that it registers and
# never removes. Its memory is freed with the state (see below), so compile a copy of pecoff.c
# that does not register it: the symbolizer makes a new state after images were loaded.
if(WIN32)
    file(READ "${_backtrace_source}/pecoff.c" _backtrace_pecoff)
    set(_register "      if (register_func)\n")
    string(FIND "${_backtrace_pecoff}" "${_register}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "libbacktrace pecoff.c changed; update the DLL notification patch in ${CMAKE_CURRENT_LIST_FILE}")
    endif()
    string(REPLACE "${_register}" "      if (register_func && 0)\n" _backtrace_pecoff "${_backtrace_pecoff}")
    file(GENERATE OUTPUT "${_backtrace_generated}/pecoff.c" CONTENT "${_backtrace_pecoff}")
    list(REMOVE_ITEM _backtrace_sources "${_backtrace_source}/pecoff.c")
    list(APPEND _backtrace_sources "${_backtrace_generated}/pecoff.c")
endif()

# Third-party code: keep its warnings out of the build log.
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(${_backtrace_sources} PROPERTIES COMPILE_OPTIONS -w)
endif()

# libbacktrace cannot free a state. This file replaces alloc.c, mmap.c, read.c and mmapio.c: it
# provides every allocation and file view, and frees all of them when the symbolizer replaces a state.
list(APPEND _backtrace_sources "${PROJECT_SOURCE_DIR}/src/firmware/crash_backtrace_memory.c")

set_source_files_properties(${_backtrace_sources} PROPERTIES
    INCLUDE_DIRECTORIES "${_backtrace_generated};${_backtrace_source}" COMPILE_DEFINITIONS _GNU_SOURCE)
set(QSIM_BACKTRACE_SOURCES ${_backtrace_sources})
set(QSIM_BACKTRACE_INCLUDE "${_backtrace_source}")
