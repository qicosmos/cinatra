cmake_minimum_required(VERSION 3.0)

project(CINATRATests CXX)

add_executable(cinatra_test unit_test.hpp utils_test.cpp response_test.cpp test.cpp)
target_link_libraries(cinatra_test ${Boost_LIBRARIES})