add_executable(test_base64 test_base64.c)
target_include_directories(test_base64 PRIVATE "${MEWNET_PACKAGE_DIR}")
target_link_libraries(test_base64 PRIVATE "${MEWNET_LIBRARY}")
if(NOT WIN32)
    target_link_libraries(test_base64 PRIVATE pthread m)
endif()
add_test(NAME base64 COMMAND test_base64)

add_executable(test_sha256 test_sha256.c)
target_include_directories(test_sha256 PRIVATE "${MEWNET_PACKAGE_DIR}")
target_link_libraries(test_sha256 PRIVATE "${MEWNET_LIBRARY}")
add_test(NAME sha256 COMMAND test_sha256)

add_executable(test_rating test_rating.c)
target_include_directories(test_rating PRIVATE "${MEWNET_PACKAGE_DIR}")
target_link_libraries(test_rating PRIVATE "${MEWNET_LIBRARY}")
if(NOT WIN32)
    target_link_libraries(test_rating PRIVATE pthread m)
endif()
add_test(NAME rating COMMAND test_rating)

set_tests_properties(base64 sha256 rating PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${MEWNET_ROOT}/dist")
