add_executable(mewnet-watch main.c)
target_link_libraries(mewnet-watch PRIVATE mewnet_subs)
target_compile_options(mewnet-watch PRIVATE -Wall -Wextra)
set_target_properties(mewnet-watch PROPERTIES
    BUILD_RPATH "${MEWNET_ROOT}/dist;${CMAKE_CURRENT_BINARY_DIR}/../../libs/mewnet-subs")
