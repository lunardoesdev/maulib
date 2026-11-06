set(MEWNET_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(SQLITE_ROOT "${MEWNET_ROOT}/third_party/sqlite3")

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

add_library(sqlite3 STATIC "${SQLITE_ROOT}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${SQLITE_ROOT}")
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_DQS=0
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_ENABLE_MATH_FUNCTIONS=1)
set_target_properties(sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(mewnet_subs SHARED
    src/subs.c
    src/util.c
    src/base64.c
    src/sha256.c
    src/fetch.c
    src/rating.c
    src/batch.c
    src/agent.c)

target_include_directories(mewnet_subs
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${MEWNET_ROOT}/libs/mewnet/include"
    PRIVATE "${SQLITE_ROOT}" "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(NOT WIN32)
    target_compile_definitions(mewnet_subs PRIVATE _GNU_SOURCE)
endif()
target_compile_options(mewnet_subs PRIVATE -Wall -Wextra)
set_target_properties(mewnet_subs PROPERTIES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    BUILD_RPATH "${MEWNET_ROOT}/dist"
    INSTALL_RPATH "$ORIGIN")

target_link_libraries(mewnet_subs PUBLIC "${MEWNET_LIBRARY}")
target_link_libraries(mewnet_subs PRIVATE sqlite3)
if(NOT WIN32)
    target_link_libraries(mewnet_subs PRIVATE pthread m dl)
endif()

add_executable(mewnet-subs tools/mewnet-subs.c)
target_link_libraries(mewnet-subs PRIVATE mewnet_subs "${MEWNET_LIBRARY}")
target_compile_options(mewnet-subs PRIVATE -Wall -Wextra)
set_target_properties(mewnet-subs PROPERTIES
    BUILD_RPATH "${MEWNET_ROOT}/dist;${CMAKE_CURRENT_BINARY_DIR}")

option(MEWNET_SUBS_BUILD_TESTS "Build mewnet_subs tests" ON)
if(MEWNET_SUBS_BUILD_TESTS)
    enable_testing()
    add_executable(test_base64 tests/test_base64.c src/base64.c src/util.c src/sha256.c)
    target_include_directories(test_base64 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MEWNET_ROOT}/libs/mewnet/include" "${SQLITE_ROOT}")
    if(NOT WIN32)
        target_compile_definitions(test_base64 PRIVATE _GNU_SOURCE)
        target_link_libraries(test_base64 PRIVATE sqlite3 pthread m)
    else()
        target_link_libraries(test_base64 PRIVATE sqlite3)
    endif()
    add_test(NAME base64 COMMAND test_base64)

    add_executable(test_sha256 tests/test_sha256.c src/sha256.c)
    target_include_directories(test_sha256 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MEWNET_ROOT}/libs/mewnet/include" "${SQLITE_ROOT}")
    if(NOT WIN32)
        target_link_libraries(test_sha256 PRIVATE pthread)
    endif()
    add_test(NAME sha256 COMMAND test_sha256)

    add_executable(test_rating tests/test_rating.c)
    target_include_directories(test_rating PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MEWNET_ROOT}/libs/mewnet/include" "${SQLITE_ROOT}")
    target_link_libraries(test_rating PRIVATE mewnet_subs sqlite3 "${MEWNET_LIBRARY}")
    if(NOT WIN32)
        target_link_libraries(test_rating PRIVATE pthread m)
    endif()
    add_test(NAME rating COMMAND test_rating)
endif()
