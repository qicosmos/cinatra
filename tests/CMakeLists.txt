set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
set(project_name test_cinatra)
add_executable(${project_name}
        test_cinatra.cpp
        main.cpp
        )
target_compile_definitions(${project_name} PRIVATE ASYNC_SIMPLE_HAS_NOT_AIO)
target_include_directories(${project_name} PRIVATE
        ${cinatra_SOURCE_DIR}/thirdparty/asio
        ${cinatra_SOURCE_DIR}/thirdparty/async_simple
)
add_test(NAME ${project_name} COMMAND test_cinatra)
target_link_libraries(${project_name} PRIVATE doctest)
