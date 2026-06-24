cmake_minimum_required(VERSION 3.15)
project(cinatra)

include_directories(include)

#the thread library of the system.
find_package(Threads REQUIRED)

if(DEFINED ENABLE_SSL AND NOT DEFINED CINATRA_ENABLE_SSL)
    set(CINATRA_ENABLE_SSL "${ENABLE_SSL}" CACHE BOOL "Enable SSL support")
endif()
option(CINATRA_ENABLE_SSL "Enable SSL support" ON)

include(cmake/find_openssl.cmake)
include(cmake/build.cmake)
include(cmake/develop.cmake)

if (BUILD_UNIT_TESTS)
    add_subdirectory(${cinatra_SOURCE_DIR}/tests)
endif ()
if (BUILD_EXAMPLES)
    add_subdirectory(${cinatra_SOURCE_DIR}/example)
endif ()
if (BUILD_PRESS_TOOL)
    add_subdirectory(${cinatra_SOURCE_DIR}/press_tool)
endif ()
