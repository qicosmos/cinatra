set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
set(project_name test_cinatra)
add_executable(${project_name}
        test_cinatra.cpp
        test_cinatra_websocket.cpp
        test_cmdline.cpp
        main.cpp
        )
target_compile_definitions(${project_name} PRIVATE ASYNC_SIMPLE_HAS_NOT_AIO INJECT_FOR_HTTP_CLIENT_TEST)
target_include_directories(${project_name} PRIVATE
        ${cinatra_SOURCE_DIR}/thirdparty/asio
        ${cinatra_SOURCE_DIR}/thirdparty/async_simple
        ${cinatra_SOURCE_DIR}/thirdparty/cmdline
        )

add_test(NAME ${project_name} COMMAND test_cinatra)

add_executable(test_coro_file test_coro_file.cpp)
target_compile_definitions(test_coro_file PRIVATE ASYNC_SIMPLE_HAS_NOT_AIO)
target_include_directories(test_coro_file PRIVATE
        ${cinatra_SOURCE_DIR}/include/cinatra
        ${cinatra_SOURCE_DIR}/thirdparty/asio
        ${cinatra_SOURCE_DIR}/thirdparty/async_simple
        )
target_link_libraries(test_coro_file doctest)

option(CINATRA_ENABLE_SSL "Enable ssl support" OFF)
if (CINATRA_ENABLE_SSL)
    message(STATUS "Use SSL")
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(test_cinatra PRIVATE -DCINATRA_ENABLE_SSL)
    target_link_libraries(test_cinatra  OpenSSL::SSL OpenSSL::Crypto)
endif ()

if (ENABLE_GZIP)
    target_link_libraries(${project_name} ${ZLIB_LIBRARIES} doctest)
else()
    target_link_libraries(test_cinatra doctest)
endif()
