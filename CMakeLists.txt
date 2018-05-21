cmake_minimum_required(VERSION 2.8)

include(CheckFunctionExists)
include(CheckLibraryExists)
include(CheckIncludeFiles)
include(CheckCSourceCompiles)

project(pg_top C)

set(PROJECT_VERSION_MAJOR 4)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_PATCH "devel")
set(PROJECT_VERSION
    ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH})

set(CMAKE_C_FLAGS "-Wall")

# Handle cmake -D command line options

if (DESTDIR)
    set(INSTALLDEST ${DESTDIR})
endif (DESTDIR)

if (NOT DEFINED ENABLE_COLOR)
    set(ENABLE_COLOR 1)
elseif(ENABLE_COLOR EQUAL 0)
    unset(ENABLE_COLOR)
else()
    set(ENABLE_COLOR "${ENABLE_COLOR}")
endif (NOT DEFINED ENABLE_COLOR)

if (NOT DEFINED ENABLE_KILL)
    set(ENABLE_KILL 1)
elseif(ENABLE_KILL EQUAL 0)
    unset(ENABLE_KILL)
else()
    set(ENABLE_KILL "${ENABLE_KILL}")
endif (NOT DEFINED ENABLE_KILL)

# Determine which machine module to build

execute_process(
    COMMAND uname -s
    OUTPUT_VARIABLE MACHINE
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(TOLOWER "${MACHINE}" MACHINE)
message("-- machine - ${MACHINE}")

# Some machines modules need additional information.

execute_process(
    COMMAND uname -r
    OUTPUT_VARIABLE OSREV
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX MATCH "^([0-9]*)" OSMAJOR "${OSREV}")
string(REPLACE "." "" OSREV "${OSREV}")
string(REPLACE "-" "" OSREV "${OSREV}")
string(REPLACE "_" "" OSREV "${OSREV}")

# Get PostgreSQL include and library directories.

execute_process(
    COMMAND pg_config --includedir
    OUTPUT_VARIABLE PGINCLUDEDIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
message("-- pg_config --includedir - ${PGINCLUDEDIR}")
if(PGINCLUDEDIR)
    set(PGINCLUDE "-I${PGINCLUDEDIR}")
endif(PGINCLUDEDIR)

execute_process(
    COMMAND pg_config --libdir
    OUTPUT_VARIABLE PGLIBDIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
message("-- pg_config --libdir - ${PGLIBDIR}")

# Check for include files.

check_include_files("stdlib.h;stdarg.h;string.h;float.h" STDC_HEADERS)

check_include_files("string.h" HAVE_STRING_H)
check_include_files("strings.h" HAVE_STRINGS_H)
check_include_files("sys/time.h;time.h" TIME_WITH_SYS_TIME)
check_include_files("sys/time.h" HAVE_SYS_TIME_H)
check_include_files("sys/resource.h" HAVE_SYS_RESOURCE_H)
check_include_files("unistd.h" HAVE_UNISTD_H)

# Check for library functions.

check_function_exists(getopt HAVE_GETOPT)
check_function_exists(memcpy HAVE_MEMCPY)
check_function_exists(setpriority HAVE_SETPRIORITY)
check_function_exists(sigaction HAVE_SIGACTION)
check_function_exists(sighold HAVE_SIGHOLD)
check_function_exists(sigprocmask HAVE_SIGPROCMASK)
check_function_exists(sigrelse HAVE_SIGRELSE)
check_function_exists(snprintf HAVE_SNPRINTF)
check_function_exists(strchr HAVE_STRCHR)
check_function_exists(strerror HAVE_STRERROR)

# Test return type of signal().

check_c_source_compiles("
#include <sys/types.h>
#include <signal.h>
int main () { return *(signal (0, 0)) (0) == 1; return 0; }
" SIGNAL_RETURN)
if(SIGNAL_RETURN)
  set(RETSIGTYPE "int")
else()
  set(RETSIGTYPE "void")
endif(SIGNAL_RETURN)

# Test whether time_t is defined.

check_c_source_compiles("
#include <sys/types.h>
int main () { time_t a; return 0; }
" TIME_T_DEFINED)
if(NOT TIME_T_DEFINED)
    set(time_t "long")
endif(NOT TIME_T_DEFINED)

# Build sigdesc.h file.

foreach(f
        /usr/include/signal.h
        /usr/include/sys/signal.h
        /usr/include/sys/iso/signal_iso.h
        /usr/include/bits/signum.h
        /usr/include/asm-generic/signal.h
       )
    execute_process(
        COMMAND grep SIGKILL ${f}
        RESULT_VARIABLE CHECK_FOR_SIGKILL
        OUTPUT_QUIET ERROR_QUIET
    )
    if(CHECK_FOR_SIGKILL EQUAL 0)
        execute_process(
            COMMAND awk -f sigconv.awk ${f}
            OUTPUT_FILE sigdesc.h
        )
        break()
    endif(CHECK_FOR_SIGKILL EQUAL 0)
endforeach(f)

# Build config.h header.

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/config.h
)

# Build manual page with appropriate machine specific notes.

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/pg_top.1.in
    ${CMAKE_CURRENT_BINARY_DIR}/pg_top.1
)
if(EXISTS "machine/m_${MACHINE}.man")
    execute_process(
        COMMAND cat machine/m_${MACHINE}.man
        OUTPUT_VARIABLE MANUAL_SUPPLIMENT
    )
    file(APPEND pg_top.1 "${MANUAL_SUPPLIMENT}")
endif(EXISTS "machine/m_${MACHINE}.man")
file(APPEND pg_top.1 "${MANUAL_SUPPLIMENT}")

# Set appropriate compile flags.

set_source_files_properties(
    color.c
    commands.c
    display.c
    pg.c
    pg_top.c
    screen.c
    sprompt.c
    username.c
    utils.c
    PROPERTIES COMPILE_FLAGS "${PGINCLUDE}"
)
set_source_files_properties(
    version.c
    PROPERTIES COMPILE_FLAGS
    "-DPACKAGE_VERSION=\\\"${PROJECT_VERSION}\\\" ${PGINCLUDE}"
)
set_source_files_properties(
    machine/m_common.c
    machine/m_remote.c
    machine/m_${MACHINE}.c
    PROPERTIES COMPILE_FLAGS "-I${CMAKE_HOME_DIRECTORY} ${PGINCLUDE}"
)

add_executable(
    ${PROJECT_NAME}
    color.c
    commands.c
    display.c
    getopt.c
    screen.c
    sprompt.c
    pg.c
    pg_top.c
    username.c
    utils.c
    version.c
    machine/m_remote.c
    machine/m_common.c
    machine/m_${MACHINE}.c
)

# Determine appropriate linker flags.

find_library(LIBPQ pq PATHS ${PGLIBDIR})
if(LIBPQ)
    target_link_libraries(${PROJECT_NAME} ${LIBPQ})
endif(LIBPQ)

find_library(LIBM m)
if(LIBM)
    target_link_libraries(${PROJECT_NAME} ${LIBM})
endif(LIBM)

find_library(LIBDL dl)
if(LIBDL)
    target_link_libraries(${PROJECT_NAME} ${LIBDL})
endif(LIBDL)

find_library(LIBINTL intl)
if(LIBINTL)
    target_link_libraries(${PROJECT_NAME} ${LIBINTL})
endif(LIBINTL)

find_library(LIBTERMCAP termcap)
if(LIBTERMCAP)
    target_link_libraries(${PROJECT_NAME} ${LIBTERMCAP})
else(LIBTERMCAP)
    find_library(LIBCURSES curses)
    if(LIBCURSES)
        target_link_libraries(${PROJECT_NAME} ${LIBCURSES})
    else(LIBCURSES)
        find_library(LIBNCURSES ncurses)
        if(LIBNCURSES)
            target_link_libraries(${PROJECT_NAME} ${LIBNCURSES})
        endif(LIBNCURSES)
    endif(LIBCURSES)
endif(LIBTERMCAP)

find_library(LIBKSTAT kstat)
if(LIBKSTAT)
    target_link_libraries(${PROJECT_NAME} ${LIBKSTAT})
endif(LIBKSTAT)

find_library(LIBELF elf)
if(LIBELF)
    target_link_libraries(${PROJECT_NAME} ${LIBELF})
endif(LIBELF)

find_library(LIBMACH mach)
if(LIBMACH)
    target_link_libraries(${PROJECT_NAME} ${LIBMACH})
endif(LIBMACH)

# FreeBSD specific libraries

if(${MACHINE} STREQUAL freebsd)
    find_library(LIBKVM kvm)
    if(LIBKVM)
        target_link_libraries(${PROJECT_NAME} ${LIBKVM})
    endif(LIBKVM)
endif(${MACHINE} STREQUAL freebsd)

install(
    PROGRAMS
    ${PROJECT_NAME}
    DESTINATION "${INSTALLDEST}/bin"
)
install(
    FILES
    pg_top.1
    DESTINATION "${INSTALLDEST}/share/man/man1"
)