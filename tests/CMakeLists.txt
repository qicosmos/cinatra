set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
set(project_name test_cinatra)
add_executable(${project_name}
        test_cinatra.cpp
        main.cpp
        )
target_include_directories(${project_name} PRIVATE
        ${cinatra_SOURCE_DIR}/thirdparty/asio
)
add_test(NAME ${project_name} COMMAND test_cinatra)
target_link_libraries(${project_name} PRIVATE doctest)
