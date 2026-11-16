cmake_minimum_required(VERSION 3.20)
project(mewnet_workspace LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(MEWNET_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(MEWNET_PACKAGE_DIR "${MEWNET_ROOT}/libs/mewnet/cmd/mewnet")

enable_testing()

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
    message(FATAL_ERROR "mewnet library not found; run 'make build' (or 'make build-mingw') first")
endif()

add_subdirectory(apps/mewnet-subs)
add_subdirectory(apps/mewnet-watch)
add_subdirectory(apps/mewnet-discovery)
if(NOT WIN32)
    add_subdirectory(apps/mewnet-cli)
endif()
option(MEWNET_BUILD_TESTS "Build C unit tests" ON)
if(MEWNET_BUILD_TESTS AND NOT WIN32)
    add_subdirectory(tests/subs)
endif()

option(MEWNET_BUILD_GTK "Build the GTK4 control panel" ON)
if(MEWNET_BUILD_GTK)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTK4 QUIET gtk4)
    endif()
    if(GTK4_FOUND)
        add_subdirectory(apps/mewnet-gtk)
    else()
        message(STATUS "GTK4 not found; mewnet-gtk will not be built")
    endif()
endif()
