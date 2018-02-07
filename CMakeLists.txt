cmake_minimum_required(VERSION 3.0)
project(cinatra)

include(ExternalProject)
find_package(Git)

if (MSVC)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++latest")
else ()
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -std=c++17")
endif ()

find_package(Boost 1.60 REQUIRED COMPONENTS system)

add_executable(cinatra main.cpp)
include_directories(${Boost_INCLUDE_DIRS} ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(cinatra ${Boost_LIBRARIES})
install(TARGETS cinatra DESTINATION include)

add_subdirectory(tests)