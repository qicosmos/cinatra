cmake_minimum_required(VERSION 3.15)
project(cinatra
        VERSION 1.0.0
        DESCRIPTION "cinatra"
        HOMEPAGE_URL "https://github.com/qicosmos/cinatra"
        LANGUAGES CXX
        )

include_directories(include)

#the thread library of the system.
find_package(Threads REQUIRED)

include(cmake/build.cmake)
include(cmake/platform.cmake)
include(cmake/develop.cmake)
include(cmake/dependency.cmake)

if (BUILD_UNIT_TESTS)
    add_subdirectory(${cinatra_SOURCE_DIR}/tests)
endif ()
if (BUILD_EXAMPLES)
    add_subdirectory(${cinatra_SOURCE_DIR}/example)
endif ()