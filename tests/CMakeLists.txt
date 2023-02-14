set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
# include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
add_executable(test_cinatra
        test_cinatra.cpp
        main.cpp
        )
add_test(NAME test_cinatra COMMAND test_cinatra)
target_link_libraries(test_cinatra PRIVATE doctest)
