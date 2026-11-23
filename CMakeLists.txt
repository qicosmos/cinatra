cmake_minimum_required(VERSION 3.15)
project(cinatra)

include_directories(include)

#the thread library of the system.
find_package(Threads REQUIRED)

include(cmake/find_openssl.cmake)
include(cmake/build.cmake)
include(cmake/develop.cmake)

option(CINATRA_ENABLE_LIBDEFLATE "Use libdeflate for buffered small gzip responses" OFF)
if(CINATRA_ENABLE_LIBDEFLATE)
    find_path(CINATRA_LIBDEFLATE_INCLUDE_DIR libdeflate.h)
    find_library(CINATRA_LIBDEFLATE_LIBRARY NAMES deflate libdeflate)
    if(NOT CINATRA_LIBDEFLATE_INCLUDE_DIR OR NOT CINATRA_LIBDEFLATE_LIBRARY)
        message(FATAL_ERROR "CINATRA_ENABLE_LIBDEFLATE requires libdeflate headers and library")
    endif()
    include_directories("${CINATRA_LIBDEFLATE_INCLUDE_DIR}")
    link_libraries("${CINATRA_LIBDEFLATE_LIBRARY}")
    add_compile_definitions(CINATRA_ENABLE_LIBDEFLATE)
endif()

option(CINATRA_TLS_BUFFERING "Use a record-sized TLS request read window and prefetched-body view" OFF)
if(CINATRA_TLS_BUFFERING)
    add_compile_definitions(CINATRA_TLS_HEADER_WINDOW=16384 CINATRA_TLS_HEADER_BODY_VIEW=1)
endif()

option(BUILD_HTTPARENA "Build the HttpArena benchmark server with SSL and GZIP (requires OpenSSL and zlib)" OFF)
if (BUILD_HTTPARENA)
    add_subdirectory(benchmark/httparena)
endif ()

if (BUILD_UNIT_TESTS)
    add_subdirectory(${cinatra_SOURCE_DIR}/tests)
endif ()
if (BUILD_EXAMPLES)
    add_subdirectory(${cinatra_SOURCE_DIR}/example)
endif ()
if (BUILD_PRESS_TOOL)
    add_subdirectory(${cinatra_SOURCE_DIR}/press_tool)
endif ()
