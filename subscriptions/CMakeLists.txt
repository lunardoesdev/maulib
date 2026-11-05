set(MILIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(SQLITE_ROOT "${MILIB_ROOT}/third_party/sqlite3")

find_library(MILIB_LIBRARY
    NAMES milib
    PATHS "${MILIB_ROOT}/dist"
    NO_DEFAULT_PATH)
if(NOT MILIB_LIBRARY)
    message(FATAL_ERROR "libmilib.so not found in ${MILIB_ROOT}/dist; run 'make build' first")
endif()

add_library(sqlite3 STATIC "${SQLITE_ROOT}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${SQLITE_ROOT}")
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_DQS=0
    SQLITE_DEFAULT_FOREIGN_KEYS=1)
set_target_properties(sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(milib_subs SHARED
    src/subs.c
    src/util.c
    src/base64.c
    src/sha256.c
    src/fetch.c
    src/rating.c)

target_include_directories(milib_subs
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${MILIB_ROOT}/include"
    PRIVATE "${SQLITE_ROOT}" "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(milib_subs PRIVATE _GNU_SOURCE)
target_compile_options(milib_subs PRIVATE -Wall -Wextra)
set_target_properties(milib_subs PROPERTIES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    BUILD_RPATH "${MILIB_ROOT}/dist"
    INSTALL_RPATH "$ORIGIN")

target_link_libraries(milib_subs PRIVATE
    sqlite3
    "${MILIB_LIBRARY}"
    pthread
    m
    dl)

add_executable(milib-subs tools/milib-subs.c)
target_link_libraries(milib-subs PRIVATE milib_subs "${MILIB_LIBRARY}")
target_compile_options(milib-subs PRIVATE -Wall -Wextra)
set_target_properties(milib-subs PROPERTIES
    BUILD_RPATH "${MILIB_ROOT}/dist;${CMAKE_CURRENT_BINARY_DIR}")

option(MILIB_SUBS_BUILD_TESTS "Build milib_subs tests" ON)
if(MILIB_SUBS_BUILD_TESTS)
    enable_testing()
    add_executable(test_base64 tests/test_base64.c src/base64.c src/util.c)
    target_include_directories(test_base64 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MILIB_ROOT}/include" "${SQLITE_ROOT}")
    target_compile_definitions(test_base64 PRIVATE _GNU_SOURCE)
    target_link_libraries(test_base64 PRIVATE sqlite3 pthread m)
    add_test(NAME base64 COMMAND test_base64)

    add_executable(test_sha256 tests/test_sha256.c src/sha256.c)
    target_include_directories(test_sha256 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MILIB_ROOT}/include" "${SQLITE_ROOT}")
    target_link_libraries(test_sha256 PRIVATE pthread)
    add_test(NAME sha256 COMMAND test_sha256)

    add_executable(test_rating tests/test_rating.c)
    target_include_directories(test_rating PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MILIB_ROOT}/include" "${SQLITE_ROOT}")
    target_link_libraries(test_rating PRIVATE milib_subs sqlite3 "${MILIB_LIBRARY}" pthread m)
    add_test(NAME rating COMMAND test_rating)
endif()
