cmake_minimum_required(VERSION 3.20)
project(maulib_apps LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# This directory holds everything that is not the library itself. The library
# sources, headers and go.mod live in the repository root; its output is dist/.
get_filename_component(MEWNET_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(MEWNET_PACKAGE_DIR "${MEWNET_ROOT}")

# The C toolchain used by Conda clang injects a sysroot whose indirect shared
# library symbols do not always match the system libm, libc, etc. The Go
# c-shared library is resolved at runtime; do not reject it at link time.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_link_options("-Wl,--allow-shlib-undefined")
endif()

# One Go c-shared library contains the Go core, all C subscription code and
# the SQLite amalgamation. Everything else links just this library.
if(NOT MEWNET_LIBRARY)
    if(WIN32)
        find_file(MEWNET_LIBRARY
            NAMES libmewnet.dll.a mewnet.lib
            PATHS "${MEWNET_ROOT}/dist/windows"
            NO_DEFAULT_PATH)
    else()
        find_library(MEWNET_LIBRARY
            NAMES mewnet
            PATHS "${MEWNET_ROOT}/dist"
            NO_DEFAULT_PATH)
    endif()
endif()
if(NOT MEWNET_LIBRARY)
    message(FATAL_ERROR "libmewnet not found in dist/; build the library from the repository root first (see README.md)")
endif()

add_subdirectory(mewnet-subs)
add_subdirectory(mewnet-watch)
add_subdirectory(mewnet-discovery)
if(NOT WIN32)
    add_subdirectory(mewnet-cli)
endif()

option(MEWNET_BUILD_GTK "Build the GTK4 control panel" ON)
if(MEWNET_BUILD_GTK)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTK4 QUIET gtk4)
    endif()
    if(GTK4_FOUND)
        add_subdirectory(mewnet-gtk)
    else()
        message(STATUS "GTK4 not found; mewnet-gtk will not be built")
    endif()
endif()
