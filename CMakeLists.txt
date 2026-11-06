cmake_minimum_required(VERSION 3.20)
project(mewnet_workspace LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

enable_testing()
add_subdirectory(libs/mewnet-subs)
add_subdirectory(apps/mewnet-watch)
