cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(httparena_cinatra LANGUAGES CXX)
endif()

option(HTTPARENA_X86_64_V3 "Target x86-64-v3 CPUs on compatible benchmark hosts" OFF)
option(HTTPARENA_FILE_IO_URING "Use native io_uring for benchmark file reads on Linux" OFF)
option(HTTPARENA_LIBDEFLATE "Use libdeflate for buffered small gzip responses" ${HTTPARENA_FILE_IO_URING})
option(HTTPARENA_TLS_BUFFERING "Use a record-sized TLS request read window and prefetched-body view" ${HTTPARENA_FILE_IO_URING})
option(HTTPARENA_OPENSSL_SOCKET_BIO "Use a Linux socket BIO for the benchmark TLS listener" OFF)

get_filename_component(CINATRA_DEFAULT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(CINATRA_SOURCE_DIR "${CINATRA_DEFAULT_SOURCE_DIR}" CACHE PATH "cinatra checkout, including its iguana submodule")
if(NOT EXISTS "${CINATRA_SOURCE_DIR}/include/cinatra/coro_http_server.hpp")
    message(FATAL_ERROR "Set CINATRA_SOURCE_DIR to a cinatra checkout")
endif()

find_package(Threads REQUIRED)
# The subscribed HttpArena profiles require TLS and gzip in every build.
message(STATUS "httparena_server: SSL and GZIP always enabled, independent of CINATRA_ENABLE_SSL")
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(httparena_server main.cpp)
set_target_properties(httparena_server PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_features(httparena_server PRIVATE cxx_std_20)
target_compile_definitions(httparena_server PRIVATE
    ASIO_STANDALONE ASYNC_SIMPLE_HAS_NOT_AIO
    CINATRA_ENABLE_SSL CINATRA_ENABLE_GZIP)
if(HTTPARENA_TLS_BUFFERING OR CINATRA_TLS_BUFFERING)
    target_compile_definitions(httparena_server PRIVATE CINATRA_TLS_HEADER_WINDOW=16384 CINATRA_TLS_HEADER_BODY_VIEW=1)
endif()
if(HTTPARENA_OPENSSL_SOCKET_BIO)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR OPENSSL_VERSION VERSION_LESS 3.0)
        message(FATAL_ERROR "HTTPARENA_OPENSSL_SOCKET_BIO requires Linux and OpenSSL 3.0 or later")
    endif()
    target_compile_definitions(httparena_server PRIVATE CINATRA_OPENSSL_SOCKET_BIO)
endif()
target_include_directories(httparena_server PRIVATE
    "${CINATRA_SOURCE_DIR}/include"
    "${CINATRA_SOURCE_DIR}/include/cinatra"
    "${CINATRA_SOURCE_DIR}/iguana")
target_link_libraries(httparena_server PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)

if(HTTPARENA_LIBDEFLATE AND NOT CINATRA_ENABLE_LIBDEFLATE)
    # The Arena runtime image need not contain a libdeflate shared library.
    # Keep the optional standalone backend reproducible with a pinned source.
    if(NOT CMAKE_C_COMPILER)
        get_filename_component(_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        get_filename_component(_cxx_name "${CMAKE_CXX_COMPILER}" NAME)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            string(REPLACE "g++" "gcc" _c_name "${_cxx_name}")
        else()
            string(REPLACE "clang++" "clang" _c_name "${_cxx_name}")
        endif()
        if("${_c_name}" STREQUAL "${_cxx_name}")
            set(_c_name cc)
        endif()
        find_program(HTTPARENA_C_COMPILER NAMES "${_c_name}" cc gcc clang HINTS "${_cxx_dir}")
        if(NOT HTTPARENA_C_COMPILER)
            message(FATAL_ERROR "The static libdeflate backend requires a C compiler")
        endif()
        set(CMAKE_C_COMPILER "${HTTPARENA_C_COMPILER}" CACHE FILEPATH "C compiler")
    endif()
    enable_language(C)
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
    set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
    set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LIBDEFLATE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(httparena_libdeflate
        URL https://codeload.github.com/ebiggers/libdeflate/tar.gz/refs/tags/v1.19
        URL_HASH SHA256=27bf62d71cd64728ff43a9feb92f2ac2f2bf748986d856133cc1e51992428c25
        TLS_VERIFY TRUE)
    FetchContent_MakeAvailable(httparena_libdeflate)
    # Match the upstream library's usual optimization policy and keep its C
    # objects independent of the C++ coroutine/LTO optimization passes.
    set_property(TARGET libdeflate_static PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(libdeflate_static PRIVATE $<$<CONFIG:Release>:-O2>)
    endif()
    target_link_libraries(httparena_server PRIVATE libdeflate_static)
    target_compile_definitions(httparena_server PRIVATE CINATRA_ENABLE_LIBDEFLATE)
endif()

if(HTTPARENA_FILE_IO_URING)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "HTTPARENA_FILE_IO_URING requires Linux")
    endif()
    find_path(HTTPARENA_URING_INCLUDE_DIR liburing.h)
    find_library(HTTPARENA_URING_LIBRARY uring)
    if(NOT HTTPARENA_URING_INCLUDE_DIR OR NOT HTTPARENA_URING_LIBRARY)
        message(FATAL_ERROR "HTTPARENA_FILE_IO_URING requires liburing headers and library")
    endif()
    target_include_directories(httparena_server PRIVATE "${HTTPARENA_URING_INCLUDE_DIR}")
    target_link_libraries(httparena_server PRIVATE "${HTTPARENA_URING_LIBRARY}")
    target_compile_definitions(httparena_server PRIVATE
        ENABLE_FILE_IO_URING ASIO_HAS_FILE ASIO_HAS_IO_URING)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Match cinatra's workaround for GCC's coroutine SLP code generation.
    target_compile_options(httparena_server PRIVATE -fno-tree-slp-vectorize)
endif()

if(HTTPARENA_X86_64_V3 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    target_compile_options(httparena_server PRIVATE -march=x86-64-v3)
endif()
