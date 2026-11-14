cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(httparena_cinatra LANGUAGES CXX)
endif()

get_filename_component(CINATRA_DEFAULT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(CINATRA_SOURCE_DIR "${CINATRA_DEFAULT_SOURCE_DIR}" CACHE PATH "cinatra checkout, including its iguana submodule")
if(NOT EXISTS "${CINATRA_SOURCE_DIR}/include/cinatra/coro_http_server.hpp")
    message(FATAL_ERROR "Set CINATRA_SOURCE_DIR to a cinatra checkout")
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(httparena_server main.cpp)
set_target_properties(httparena_server PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_features(httparena_server PRIVATE cxx_std_20)
target_compile_definitions(httparena_server PRIVATE
    ASIO_STANDALONE ASYNC_SIMPLE_HAS_NOT_AIO
    CINATRA_ENABLE_SSL CINATRA_ENABLE_GZIP)
target_include_directories(httparena_server PRIVATE
    "${CINATRA_SOURCE_DIR}/include"
    "${CINATRA_SOURCE_DIR}/include/cinatra"
    "${CINATRA_SOURCE_DIR}/iguana")
target_link_libraries(httparena_server PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Match cinatra's workaround for GCC's coroutine SLP code generation.
    target_compile_options(httparena_server PRIVATE -fno-tree-slp-vectorize)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    target_compile_options(httparena_server PRIVATE -march=x86-64-v3)
endif()
