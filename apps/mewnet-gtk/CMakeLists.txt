find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

add_executable(mewnet-gtk main.c)
target_link_libraries(mewnet-gtk PRIVATE mewnet_subs PkgConfig::GTK4)
target_compile_options(mewnet-gtk PRIVATE -Wall -Wextra)
# The Go core is built with cgo; GTK lives in the same process. Some toolchains
# (notably conda clang) inject a sysroot whose indirect shared-library symbols
# do not match the system GTK stack, so let the runtime resolver handle them.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_link_options(mewnet-gtk PRIVATE "-Wl,--allow-shlib-undefined")
endif()
set_target_properties(mewnet-gtk PROPERTIES
    BUILD_RPATH "${CMAKE_SOURCE_DIR}/dist;${CMAKE_BINARY_DIR}/libs/mewnet-subs")
