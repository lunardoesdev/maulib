add_executable(mewnet-cli main.c)
target_link_libraries(mewnet-cli PRIVATE mewnet_subs pthread)
target_compile_options(mewnet-cli PRIVATE -Wall -Wextra)
set_target_properties(mewnet-cli PROPERTIES
    BUILD_RPATH "${CMAKE_SOURCE_DIR}/dist;${CMAKE_BINARY_DIR}/libs/mewnet-subs")
